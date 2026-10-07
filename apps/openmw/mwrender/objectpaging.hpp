#ifndef OPENMW_MWRENDER_OBJECTPAGING_H
#define OPENMW_MWRENDER_OBJECTPAGING_H

#include <components/esm3/refnum.hpp>
#include <components/resource/resourcemanager.hpp>
#include <components/terrain/quadtreeworld.hpp>

#include <atomic>
#include <map>
#include <mutex>
#include <string>

#include <osg/observer_ptr>

namespace Resource
{
    class SceneManager;
}

namespace SceneUtil
{
    class OcclusionCuller;
}

namespace MWRender
{
    class OccluderRegistry;

    typedef std::tuple<osg::Vec2f, float, bool> ChunkId; // Center, Size, ActiveGrid

    class ObjectPaging : public Resource::NodeResourceManager<ChunkId>, public Terrain::QuadTreeWorld::ChunkManager
    {
    public:
        ObjectPaging(Resource::SceneManager* sceneManager, ESM::RefId worldspace);
        ~ObjectPaging() = default;

        osg::ref_ptr<osg::Node> getChunk(float size, const osg::Vec2f& center, unsigned char lod, unsigned int lodFlags,
            bool activeGrid, const osg::Vec3f& viewPoint, bool compile) override;

        osg::ref_ptr<osg::Node> createChunk(float size, const osg::Vec2f& center, bool activeGrid,
            const osg::Vec3f& viewPoint, bool compile, unsigned char lod);

        unsigned int getNodeMask() override;

        /// @return true if view needs rebuild
        bool enableObject(int type, ESM::RefNum refnum, const osg::Vec3f& pos, const osg::Vec2i& cell, bool enabled);

        /// @return true if view needs rebuild
        bool blacklistObject(int type, ESM::RefNum refnum, const osg::Vec3f& pos, const osg::Vec2i& cell);

        void clear();

        /// Must be called after clear() before rendering starts.
        /// @return true if view needs rebuild
        bool unlockCache();

        void reportStats(unsigned int frameNumber, osg::Stats* stats) const override;

        void getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out);

        /// OpenMGE XE: chunks created from now on build occluder meshes for their buildings and are tested against
        /// the software occlusion buffer (the registry, owned by the RenderingManager, feeds the nearest-first
        /// occluder pre-pass).
        void setOcclusionCuller(
            SceneUtil::OcclusionCuller* culler, OccluderRegistry* registry, unsigned int maxTriangles)
        {
            mOcclusionCuller = culler;
            mOccluderRegistry = registry;
            mMaxTriangles = maxTriangles;
        }

    private:
        Resource::SceneManager* mSceneManager;
        SceneUtil::OcclusionCuller* mOcclusionCuller = nullptr;
        OccluderRegistry* mOccluderRegistry = nullptr;
        unsigned int mMaxTriangles = 30000;
        bool mActiveGrid;
        bool mDebugBatches;
        float mMergeFactor;
        float mMinSize;
        float mMinSizeMergeFactor;
        float mMinSizeCostMultiplier;
        float mReflectionStaticsDistance; // [Water] reflection statics distance, 0 = no limit

        std::mutex mRefTrackerMutex;
        struct RefTracker
        {
            std::set<ESM::RefNum> mDisabled;
            std::set<ESM::RefNum> mBlacklist;
            bool operator==(const RefTracker& other) const
            {
                return mDisabled == other.mDisabled && mBlacklist == other.mBlacklist;
            }
        };
        RefTracker mRefTracker;
        RefTracker mRefTrackerNew;
        bool mRefTrackerLocked;

        const RefTracker& getRefTracker() const { return mRefTracker; }
        RefTracker& getWritableRefTracker() { return mRefTrackerLocked ? mRefTrackerNew : mRefTracker; }

        std::mutex mSizeCacheMutex;
        typedef std::map<ESM::RefNum, float> SizeCache;
        SizeCache mSizeCache;

        std::mutex mLODNameCacheMutex;
        typedef std::pair<std::string, unsigned char> LODNameCacheKey; // Key: mesh name, lod level
        using LODNameCache = std::map<LODNameCacheKey, VFS::Path::Normalized>; // Cache: key, mesh name to use
        LODNameCache mLODNameCache;

        // OpenMGE XE automatic LOD: simplified copies of the models in distant chunks, per model and detail level.
        // An entry is valid while its source template is the one the scene manager hands out.
        struct AutoLodEntry
        {
            osg::observer_ptr<osg::Node> mSource;
            osg::ref_ptr<const osg::Node> mLod; // nullptr: the model can't be simplified usefully at this level
            unsigned int mTrianglesBefore = 0;
            unsigned int mTrianglesAfter = 0;
        };
        using AutoLodKey = std::pair<std::string, int>; // Key: mesh name, detail level
        std::mutex mAutoLodMutex;
        std::map<AutoLodKey, AutoLodEntry> mAutoLodCache;
        unsigned int mAutoLodInsertions = 0;
        std::atomic<unsigned int> mAutoLodMeshes{ 0 };
        std::atomic<unsigned long long> mAutoLodTrianglesBefore{ 0 };
        std::atomic<unsigned long long> mAutoLodTrianglesAfter{ 0 };

        /// @param mayCreate false while drawing waits for the chunk: only an already made copy is used.
        osg::ref_ptr<const osg::Node> getAutoLod(
            const VFS::Path::Normalized& model, const osg::Node& source, int level, bool mayCreate);
        void eraseAutoLodEntry(std::map<AutoLodKey, AutoLodEntry>::iterator it);
    };

    struct RefnumMarker
    {
        ESM::RefNum mRefnum;
        unsigned int mNumVertices = 0;
    };
}

#endif
