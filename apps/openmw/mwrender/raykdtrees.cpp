#include "raykdtrees.hpp"

#include <algorithm>

#include <osg/Geometry>
#include <osg/KdTree>

#include <components/sceneutil/workqueue.hpp>

namespace MWRender
{
    namespace
    {
        // Below this a mesh is cheap to test triangle by triangle (2048 triangles is about 15 microseconds) and a
        // tree is not worth its memory.
        constexpr unsigned int sMinTriangles = 2048;

        // Builds waiting at once. Rays reach a handful of big meshes; this only bounds a burst after a teleport.
        constexpr std::size_t sMaxPending = 32;

        unsigned int countTriangles(const osg::Geometry& geometry)
        {
            unsigned int count = 0;
            for (const osg::ref_ptr<osg::PrimitiveSet>& primitiveSet : geometry.getPrimitiveSetList())
            {
                const unsigned int indices = primitiveSet->getNumIndices();
                switch (primitiveSet->getMode())
                {
                    case GL_TRIANGLES:
                        count += indices / 3;
                        break;
                    case GL_TRIANGLE_STRIP:
                    case GL_TRIANGLE_FAN:
                        if (indices > 2)
                            count += indices - 2;
                        break;
                    default:
                        break;
                }
            }
            return count;
        }
    }

    struct RayKdTrees::Job final : SceneUtil::WorkItem
    {
        explicit Job(osg::Geometry* geometry)
            : mGeometry(geometry)
        {
        }

        void doWork() override
        {
            // Reads the vertex and index arrays only, which nothing changes for a static mesh, so it is safe
            // alongside the cull and draw threads.
            osg::ref_ptr<osg::KdTree> tree = new osg::KdTree;
            osg::KdTree::BuildOptions options;
            if (tree->build(options, mGeometry.get()))
                mTree = tree;
        }

        osg::ref_ptr<osg::Geometry> mGeometry;
        osg::ref_ptr<osg::KdTree> mTree;
    };

    RayKdTrees::RayKdTrees() = default;

    RayKdTrees::~RayKdTrees() = default;

    void RayKdTrees::visit(osg::Drawable& drawable, SceneUtil::WorkQueue* queue)
    {
        if (queue == nullptr)
            return;

        osg::Geometry* geometry = drawable.asGeometry();
        // a shape is either the tree already, or something this must not replace
        if (geometry == nullptr || geometry->getShape() != nullptr)
            return;
        if (geometry->getDataVariance() == osg::Object::DYNAMIC)
            return;
        if (dynamic_cast<const osg::Vec3Array*>(geometry->getVertexArray()) == nullptr)
            return;
        if (countTriangles(*geometry) < sMinTriangles)
            return;
        if (mPending.size() >= sMaxPending)
            return;
        if (std::any_of(mPending.begin(), mPending.end(),
                [&](const std::shared_ptr<Job>& job) { return job->mGeometry == geometry; }))
            return;

        const auto rejected = mRejected.find(geometry);
        if (rejected != mRejected.end())
        {
            if (rejected->second.valid())
                return;
            // a different mesh at the address of one that was freed
            mRejected.erase(rejected);
        }

        auto job = std::make_shared<Job>(geometry);
        mPending.push_back(job);
        queue->addWorkItem(job);
    }

    void RayKdTrees::collect()
    {
        if (mPending.empty())
            return;

        std::erase_if(mPending, [&](const std::shared_ptr<Job>& job) {
            if (!job->isDone())
                return false;
            if (job->mTree)
            {
                if (job->mGeometry->getShape() == nullptr)
                    job->mGeometry->setShape(job->mTree);
            }
            else
            {
                if (mRejected.size() > 1024)
                    std::erase_if(mRejected, [](const auto& entry) { return !entry.second.valid(); });
                mRejected.emplace(job->mGeometry.get(), job->mGeometry.get());
            }
            return true;
        });
    }
}
