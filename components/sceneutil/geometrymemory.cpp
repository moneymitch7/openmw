#include "geometrymemory.hpp"

#include <osg/Geometry>
#include <osg/NodeVisitor>
#include <osg/PrimitiveSet>
#include <osg/StateSet>
#include <osg/Texture>

namespace SceneUtil
{
    namespace
    {
        class GeometryMemoryVisitor : public osg::NodeVisitor
        {
        public:
            GeometryMemoryVisitor(std::unordered_set<const osg::BufferData*>& seen,
                std::unordered_set<const osg::Texture*>& textures, std::size_t& bytes)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mSeen(seen)
                , mTextures(textures)
                , mBytes(bytes)
            {
                // everything, hidden or not
                setNodeMaskOverride(~0u);
            }

            void apply(osg::Node& node) override
            {
                addStateSet(node.getStateSet());
                traverse(node);
            }

            void apply(osg::Drawable& drawable) override
            {
                addStateSet(drawable.getStateSet());
                if (osg::Geometry* geometry = drawable.asGeometry())
                {
                    addData(geometry->getVertexArray());
                    addData(geometry->getNormalArray());
                    addData(geometry->getColorArray());
                    addData(geometry->getSecondaryColorArray());
                    addData(geometry->getFogCoordArray());
                    for (unsigned int i = 0; i < geometry->getNumTexCoordArrays(); ++i)
                        addData(geometry->getTexCoordArray(i));
                    for (unsigned int i = 0; i < geometry->getNumVertexAttribArrays(); ++i)
                        addData(geometry->getVertexAttribArray(i));
                    for (unsigned int i = 0; i < geometry->getNumPrimitiveSets(); ++i)
                        if (osg::DrawElements* elements = geometry->getPrimitiveSet(i)->getDrawElements())
                            addData(elements);
                }
            }

        private:
            void addData(const osg::BufferData* data)
            {
                if (data != nullptr && mSeen.insert(data).second)
                    mBytes += data->getTotalDataSize();
            }

            void addStateSet(const osg::StateSet* stateSet)
            {
                if (stateSet == nullptr)
                    return;
                for (const auto& unit : stateSet->getTextureAttributeList())
                    for (const auto& [type, attribute] : unit)
                        if (type.first == osg::StateAttribute::TEXTURE)
                            if (const auto* texture = dynamic_cast<const osg::Texture*>(attribute.first.get()))
                                mTextures.insert(texture);
            }

            std::unordered_set<const osg::BufferData*>& mSeen;
            std::unordered_set<const osg::Texture*>& mTextures;
            std::size_t& mBytes;
        };
    }

    void GeometryMemory::add(const osg::Object* object)
    {
        const osg::Node* node = object != nullptr ? object->asNode() : nullptr;
        if (node == nullptr)
            return;
        GeometryMemoryVisitor visitor(mSeen, mTextures, mBytes);
        const_cast<osg::Node*>(node)->accept(visitor);
    }
}
