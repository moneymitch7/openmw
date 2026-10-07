#include "autolod.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <typeinfo>
#include <vector>

#include <osg/Geometry>
#include <osg/PrimitiveSet>
#include <osg/TriangleIndexFunctor>

#include <meshoptimizer.h>

namespace SceneUtil
{
    namespace
    {
        struct CollectTriangles
        {
            std::vector<unsigned int>* mIndices = nullptr;

            void operator()(unsigned int a, unsigned int b, unsigned int c)
            {
                if (a == b || b == c || a == c)
                    return;
                mIndices->push_back(a);
                mIndices->push_back(b);
                mIndices->push_back(c);
            }
        };

        bool isTriangleMode(GLenum mode)
        {
            switch (mode)
            {
                case GL_TRIANGLES:
                case GL_TRIANGLE_STRIP:
                case GL_TRIANGLE_FAN:
                case GL_QUADS:
                case GL_QUAD_STRIP:
                case GL_POLYGON:
                    return true;
                default:
                    return false;
            }
        }

        // Per-vertex arrays get remapped, overall ones copied; anything else makes the geometry unsuitable.
        bool isSupportedArray(const osg::Array* array, unsigned int vertexCount)
        {
            if (array == nullptr)
                return true;
            switch (array->getBinding())
            {
                case osg::Array::BIND_OVERALL:
                    return array->getNumElements() >= 1;
                case osg::Array::BIND_PER_VERTEX:
                case osg::Array::BIND_UNDEFINED:
                    return array->getNumElements() >= vertexCount;
                default:
                    return false;
            }
        }

        // A new array of the same type holding only the vertices in newToOld, in that order. Never shares a buffer
        // object with the source, so the template's VBOs stay untouched.
        osg::ref_ptr<osg::Array> remapArray(const osg::Array* src, const std::vector<unsigned int>& newToOld)
        {
            if (src == nullptr)
                return nullptr;
            osg::ref_ptr<osg::Array> dst = static_cast<osg::Array*>(src->cloneType());
            const bool overall = src->getBinding() == osg::Array::BIND_OVERALL;
            const unsigned int count = overall ? 1u : static_cast<unsigned int>(newToOld.size());
            dst->resizeArray(count);
            const unsigned int elementSize = src->getElementSize();
            const char* from = static_cast<const char*>(src->getDataPointer());
            char* to = static_cast<char*>(const_cast<void*>(dst->getDataPointer()));
            if (overall)
                std::memcpy(to, from, elementSize);
            else
                for (unsigned int i = 0; i < count; ++i)
                    std::memcpy(to + i * elementSize, from + newToOld[i] * elementSize, elementSize);
            dst->setBinding(src->getBinding());
            dst->setNormalize(src->getNormalize());
            dst->setPreserveDataType(src->getPreserveDataType());
            return dst;
        }

        osg::ref_ptr<osg::Geometry> simplifyGeometry(
            const osg::Geometry& geom, float maxError, unsigned int minTriangles, AutoLodStats* stats)
        {
            if (geom.getUpdateCallback() || geom.getCullCallback() || geom.getDrawCallback() || geom.getEventCallback())
                return nullptr;

            const osg::Vec3Array* vertices = dynamic_cast<const osg::Vec3Array*>(geom.getVertexArray());
            if (vertices == nullptr || vertices->empty())
                return nullptr;
            const unsigned int vertexCount = vertices->getNumElements();

            if (geom.getNumPrimitiveSets() == 0)
                return nullptr;
            for (unsigned int i = 0; i < geom.getNumPrimitiveSets(); ++i)
            {
                const osg::PrimitiveSet* prim = geom.getPrimitiveSet(i);
                if (prim == nullptr || !isTriangleMode(prim->getMode()) || prim->getNumInstances() > 1)
                    return nullptr;
            }

            if (!isSupportedArray(geom.getNormalArray(), vertexCount)
                || !isSupportedArray(geom.getColorArray(), vertexCount)
                || !isSupportedArray(geom.getSecondaryColorArray(), vertexCount)
                || !isSupportedArray(geom.getFogCoordArray(), vertexCount))
                return nullptr;
            for (unsigned int i = 0; i < geom.getNumTexCoordArrays(); ++i)
                if (!isSupportedArray(geom.getTexCoordArray(i), vertexCount))
                    return nullptr;
            for (unsigned int i = 0; i < geom.getNumVertexAttribArrays(); ++i)
                if (!isSupportedArray(geom.getVertexAttribArray(i), vertexCount))
                    return nullptr;

            std::vector<unsigned int> indices;
            {
                osg::TriangleIndexFunctor<CollectTriangles> functor;
                functor.mIndices = &indices;
                const_cast<osg::Geometry&>(geom).accept(functor);
            }
            const size_t triangles = indices.size() / 3;
            if (triangles < std::max(1u, minTriangles))
                return nullptr;
            for (unsigned int index : indices)
                if (index >= vertexCount)
                    return nullptr;

            std::vector<unsigned int> simplified(indices.size());
            float resultError = 0.f;
            // meshoptimizer measures the error at the vertices it keeps; across the longer triangles between them a
            // curved surface can be up to about twice as far off, so ask for half.
            size_t newCount = meshopt_simplify(simplified.data(), indices.data(), indices.size(),
                vertices->front().ptr(), vertexCount, sizeof(osg::Vec3f), 0, maxError * 0.5f,
                meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute, &resultError);
            // Not worth a copy of its own (or so small it would vanish; keep it rather than drop it).
            if (newCount == 0 || newCount > indices.size() * 9 / 10)
                return nullptr;
            simplified.resize(newCount);
            meshopt_optimizeVertexCache(simplified.data(), simplified.data(), newCount, vertexCount);

            // Keep only the vertices still used, in first-use order (which also suits the vertex fetch).
            std::vector<unsigned int> oldToNew(vertexCount, std::numeric_limits<unsigned int>::max());
            std::vector<unsigned int> newToOld;
            newToOld.reserve(vertexCount);
            for (unsigned int& index : simplified)
            {
                unsigned int& mapped = oldToNew[index];
                if (mapped == std::numeric_limits<unsigned int>::max())
                {
                    mapped = static_cast<unsigned int>(newToOld.size());
                    newToOld.push_back(index);
                }
                index = mapped;
            }

            osg::ref_ptr<osg::Geometry> result = new osg::Geometry;
            result->setName(geom.getName());
            result->setStateSet(const_cast<osg::StateSet*>(geom.getStateSet()));
            result->setUserDataContainer(const_cast<osg::UserDataContainer*>(geom.getUserDataContainer()));
            result->setNodeMask(geom.getNodeMask());
            result->setDataVariance(geom.getDataVariance());
            result->setCullingActive(geom.getCullingActive());
            result->setUseDisplayList(geom.getUseDisplayList());
            result->setUseVertexBufferObjects(geom.getUseVertexBufferObjects());

            result->setVertexArray(remapArray(vertices, newToOld));
            result->setNormalArray(remapArray(geom.getNormalArray(), newToOld));
            result->setColorArray(remapArray(geom.getColorArray(), newToOld));
            result->setSecondaryColorArray(remapArray(geom.getSecondaryColorArray(), newToOld));
            result->setFogCoordArray(remapArray(geom.getFogCoordArray(), newToOld));
            for (unsigned int i = 0; i < geom.getNumTexCoordArrays(); ++i)
                if (const osg::Array* array = geom.getTexCoordArray(i))
                    result->setTexCoordArray(i, remapArray(array, newToOld));
            for (unsigned int i = 0; i < geom.getNumVertexAttribArrays(); ++i)
                if (const osg::Array* array = geom.getVertexAttribArray(i))
                    result->setVertexAttribArray(i, remapArray(array, newToOld));

            if (newToOld.size() <= std::numeric_limits<unsigned short>::max())
            {
                osg::ref_ptr<osg::DrawElementsUShort> elements
                    = new osg::DrawElementsUShort(GL_TRIANGLES, simplified.begin(), simplified.end());
                result->addPrimitiveSet(elements);
            }
            else
            {
                osg::ref_ptr<osg::DrawElementsUInt> elements
                    = new osg::DrawElementsUInt(GL_TRIANGLES, simplified.begin(), simplified.end());
                result->addPrimitiveSet(elements);
            }

            if (stats != nullptr)
            {
                stats->mTrianglesBefore += static_cast<unsigned int>(triangles);
                stats->mTrianglesAfter += static_cast<unsigned int>(newCount / 3);
            }
            return result;
        }

        // Copies the node graph, putting simplified geometry in place of the meshes that allow it and sharing every
        // other drawable with the source.
        class SimplifyCopyOp : public osg::CopyOp
        {
        public:
            float mMaxError;
            unsigned int mMinTriangles;
            AutoLodStats* mStats;
            osg::Node::NodeMask mMask;
            mutable bool mChanged = false;

            SimplifyCopyOp(float maxError, unsigned int minTriangles, AutoLodStats* stats, osg::Node::NodeMask mask)
                : osg::CopyOp(osg::CopyOp::DEEP_COPY_NODES)
                , mMaxError(maxError)
                , mMinTriangles(minTriangles)
                , mStats(stats)
                , mMask(mask)
            {
            }

            osg::Node* operator()(const osg::Node* node) const override
            {
                if (node == nullptr)
                    return nullptr;
                // Not drawn (collision shapes and the like): share it as it is.
                if (!(node->getNodeMask() & mMask))
                    return const_cast<osg::Node*>(node);
                if (const osg::Drawable* drawable = node->asDrawable())
                    return operator()(drawable);
                return osg::clone(node, *this);
            }

            osg::Drawable* operator()(const osg::Drawable* drawable) const override
            {
                if (drawable == nullptr)
                    return nullptr;
                // Exactly osg::Geometry: skinned (RigGeometry), morphed and particle drawables derive from it or
                // from Drawable and must stay as they are.
                if (typeid(*drawable) == typeid(osg::Geometry))
                {
                    osg::ref_ptr<osg::Geometry> simplified = simplifyGeometry(
                        static_cast<const osg::Geometry&>(*drawable), mMaxError, mMinTriangles, mStats);
                    if (simplified)
                    {
                        mChanged = true;
                        return simplified.release();
                    }
                }
                return const_cast<osg::Drawable*>(drawable);
            }
        };
    }

    namespace
    {
        std::atomic<unsigned long long> sAutoLodMicroseconds{ 0 };
    }

    unsigned long long getAutoLodMicroseconds()
    {
        return sAutoLodMicroseconds.load();
    }

    osg::ref_ptr<osg::Node> createSimplifiedCopy(
        const osg::Node& node, float maxError, unsigned int minTriangles, AutoLodStats* stats, osg::Node::NodeMask mask)
    {
        if (!(maxError > 0.f))
            return nullptr;
        const auto start = std::chrono::steady_clock::now();
        SimplifyCopyOp copyOp(maxError, minTriangles, stats, mask);
        osg::ref_ptr<osg::Node> copy = copyOp(&node);
        sAutoLodMicroseconds += static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        if (!copyOp.mChanged || copy == nullptr || copy.get() == &node)
            return nullptr;
        return copy;
    }
}
