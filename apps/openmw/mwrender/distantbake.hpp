#ifndef OPENMW_MWRENDER_DISTANTBAKE_H
#define OPENMW_MWRENDER_DISTANTBAKE_H

#include <osg/Matrixf>
#include <osg/Node>
#include <osg/StateSet>
#include <osg/Texture2DArray>
#include <osg/ref_ptr>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace VFS
{
    class Manager;
}

namespace Resource
{
    class SceneManager;
}

namespace MWRender
{
    /// OpenMGE XE distant land, stage 2: the distant texture set made by openmw-distantlandtool
    /// (distantland/textures.bin in the data folders): small copies of the base textures of the models placed
    /// outside, in texture arrays. Object paging's distant chunks draw the objects whose textures are all in it from
    /// one merged shape per array and kind (solid, alpha tested, two-sided) instead of a draw per texture.
    class DistantTextureSet
    {
    public:
        /// nullptr when there is no set (or it can't be read)
        static std::unique_ptr<DistantTextureSet> load(const VFS::Manager& vfs);

        struct Entry
        {
            std::uint16_t mArray;
            std::uint16_t mLayer;
        };
        /// @param path a texture's path as the data folders name it (textures/...)
        const Entry* find(const std::string& path) const;

        enum Kind
        {
            Solid = 0,
            AlphaTested = 1,
            TwoSided = 2,
        };
        static constexpr int sKinds = 4;

        /// The state set (with its shaders) of an array's shapes of a kind (Kind bits).
        osg::StateSet* getStateSet(Resource::SceneManager& sceneManager, std::size_t array, int kind);

        std::size_t getNumArrays() const { return mArrays.size(); }
        std::size_t getNumTextures() const { return mEntries.size(); }

    private:
        std::unordered_map<std::string, Entry> mEntries;
        std::vector<osg::ref_ptr<osg::Texture2DArray>> mArrays;
        std::mutex mStateSetsMutex;
        std::map<std::pair<std::size_t, int>, osg::ref_ptr<osg::StateSet>> mStateSets;
    };

    /// Merges the instances of a distant chunk whose models can be (static, every shape textured from the set, no
    /// blending, no glow) into one shape per array and kind.
    class DistantChunkBaker
    {
    public:
        DistantChunkBaker(DistantTextureSet& set, Resource::SceneManager& sceneManager);

        /// @return whether the instance of @a model was taken (else draw it as before)
        bool add(const osg::Node& model, const osg::Matrixf& matrix);

        /// The merged shapes, added to @a parent.
        void build(osg::Group& parent);

        std::size_t getNumInstances() const { return mInstances; }

        /// Forgets the models analysed (when object paging's caches are cleared).
        static void clearCache();

    private:
        struct Buffer
        {
            std::vector<osg::Vec3f> mPositions;
            std::vector<osg::Vec3f> mNormals;
            std::vector<std::uint32_t> mColors; // RGBA8
            std::vector<osg::Vec2f> mTexCoords;
            std::vector<float> mLayers;
            std::vector<std::uint32_t> mIndices;
        };
        DistantTextureSet& mSet;
        Resource::SceneManager& mSceneManager;
        std::map<std::pair<std::size_t, int>, Buffer> mBuffers;
        std::size_t mInstances = 0;
    };
}

#endif
