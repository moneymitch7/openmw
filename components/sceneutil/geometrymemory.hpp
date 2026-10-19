#ifndef OPENMW_COMPONENTS_SCENEUTIL_GEOMETRYMEMORY_H
#define OPENMW_COMPONENTS_SCENEUTIL_GEOMETRYMEMORY_H

#include <cstddef>
#include <unordered_set>

namespace osg
{
    class BufferData;
    class Object;
    class Texture;
}

namespace SceneUtil
{
    /// OpenMGE XE: how much vertex and index data (what goes to the GPU as buffers) and how many textures a set of
    /// scene graphs hold, counting what they share once. For the performance log: which cache holds the memory.
    class GeometryMemory
    {
    public:
        /// Adds what @a object (a node and everything under it) holds.
        void add(const osg::Object* object);

        std::size_t getBytes() const { return mBytes; }
        std::size_t getTextures() const { return mTextures.size(); }

    private:
        std::unordered_set<const osg::BufferData*> mSeen;
        std::unordered_set<const osg::Texture*> mTextures;
        std::size_t mBytes = 0;
    };
}

#endif
