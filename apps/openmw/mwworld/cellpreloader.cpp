#include "cellpreloader.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <span>
#include <thread>

#include <osg/Stats>

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/loadinglistener/loadinglistener.hpp>
#include <components/loadinglistener/reporter.hpp>
#include <components/misc/constants.hpp>
#include <components/misc/pathhelpers.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/resource/bulletshapemanager.hpp>
#include <components/resource/keyframemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/settings/values.hpp>
#include <components/terrain/view.hpp>
#include <components/terrain/world.hpp>
#include <components/vfs/manager.hpp>

#include "../mwrender/landmanager.hpp"

#include "cellstore.hpp"
#include "class.hpp"

namespace MWWorld
{
    namespace
    {
        bool contains(std::span<const PositionCellGrid> positions, const PositionCellGrid& contained, float tolerance)
        {
            const float squaredTolerance = tolerance * tolerance;
            const auto predicate = [&](const PositionCellGrid& v) {
                return (contained.mPosition - v.mPosition).length2() < squaredTolerance
                    && contained.mCellBounds == v.mCellBounds;
            };
            return std::ranges::any_of(positions, predicate);
        }

        bool contains(
            std::span<const PositionCellGrid> container, std::span<const PositionCellGrid> contained, float tolerance)
        {
            const auto predicate = [&](const PositionCellGrid& v) { return contains(container, v, tolerance); };
            return std::ranges::all_of(contained, predicate);
        }
    }

    struct ListModelsVisitor
    {
        bool operator()(const MWWorld::ConstPtr& ptr)
        {
            ptr.getClass().getModelsToPreload(ptr, mOut);

            return true;
        }

        std::vector<VFS::Path::NormalizedView>& mOut;
    };

    /// Worker thread item: preload models in a cell.
    class PreloadItem : public SceneUtil::WorkItem
    {
    public:
        /// Constructor to be called from the main thread.
        explicit PreloadItem(MWWorld::CellStore* cell, Resource::SceneManager* sceneManager,
            Resource::BulletShapeManager* bulletShapeManager, Resource::KeyframeManager* keyframeManager,
            Terrain::World* terrain, MWRender::LandManager* landManager, bool preloadInstances)
            : mIsExterior(cell->getCell()->isExterior())
            , mCellLocation(cell->getCell()->getExteriorCellLocation())
            , mCellId(cell->getCell()->getId())
            , mSceneManager(sceneManager)
            , mBulletShapeManager(bulletShapeManager)
            , mKeyframeManager(keyframeManager)
            , mTerrain(terrain)
            , mLandManager(landManager)
            , mPreloadInstances(preloadInstances)
            , mAbort(false)
        {
            mTerrainView = mTerrain->createView();

            ListModelsVisitor visitor{ mMeshes };
            cell->forEachConst(visitor);
        }

        void abort() override { mAbort = true; }

        /// Preload work to be called from the worker thread.
        void doWork() override
        {
            if (mIsExterior)
            {
                try
                {
                    mTerrain->cacheCell(mTerrainView.get(), mCellLocation.mX, mCellLocation.mY);
                    mPreloadedOwnedObjects.insert(mLandManager->getLand(mCellLocation));
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "Failed to cache terrain for exterior cell " << mCellLocation << ": "
                                        << e.what();
                }
            }

            VFS::Path::Normalized mesh;
            VFS::Path::Normalized kfname;
            for (VFS::Path::NormalizedView path : mMeshes)
            {
                if (mAbort)
                    break;

                try
                {
                    const VFS::Manager& vfs = *mSceneManager->getVFS();
                    mesh = Misc::ResourceHelpers::correctMeshPath(path);
                    mesh = Misc::ResourceHelpers::correctActorModelPath(mesh, &vfs);

                    if (!vfs.exists(mesh))
                        continue;

                    constexpr VFS::Path::ExtensionView nif("nif");
                    if (Misc::getFileName(mesh).starts_with('x') && mesh.extension() == nif)
                    {
                        kfname = mesh;
                        constexpr VFS::Path::ExtensionView kf("kf");
                        kfname.changeExtension(kf);
                        if (vfs.exists(kfname))
                            mPreloadedOwnedObjects.insert(mKeyframeManager->get(kfname));
                    }

                    mPreloadedObjects.insert(mSceneManager->getTemplate(mesh));
                    if (mPreloadInstances)
                        mPreloadedOwnedObjects.insert(mBulletShapeManager->cacheInstance(mesh));
                    else
                        mPreloadedOwnedObjects.insert(mBulletShapeManager->getShape(mesh));
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "Failed to preload mesh \"" << path << "\" from cell " << mCellId << ": "
                                        << e.what();
                }
            }
        }

    private:
        bool mIsExterior;
        ESM::ExteriorCellLocation mCellLocation;
        ESM::RefId mCellId;
        std::vector<VFS::Path::NormalizedView> mMeshes;
        Resource::SceneManager* mSceneManager;
        Resource::BulletShapeManager* mBulletShapeManager;
        Resource::KeyframeManager* mKeyframeManager;
        Terrain::World* mTerrain;
        MWRender::LandManager* mLandManager;
        bool mPreloadInstances;

        std::atomic<bool> mAbort;

        osg::ref_ptr<Terrain::View> mTerrainView;

        // Keep the loaded objects alive for as long as this cell is preloaded. Two sets
        // because the render-owned ones are still osg::Object; they merge once those move
        // behind the render backend.
        std::set<osg::ref_ptr<const osg::Object>> mPreloadedObjects;
        std::set<std::shared_ptr<const void>> mPreloadedOwnedObjects;
    };

    class TerrainPreloadItem : public SceneUtil::WorkItem
    {
    public:
        explicit TerrainPreloadItem(const std::vector<osg::ref_ptr<Terrain::View>>& views, Terrain::World* world,
            std::span<const PositionCellGrid> preloadPositions)
            : mAbort(false)
            , mTerrainViews(views)
            , mWorld(world)
            , mPreloadPositions(preloadPositions.begin(), preloadPositions.end())
        {
        }

        void doWork() override
        {
            for (unsigned int i = 0; i < mTerrainViews.size() && i < mPreloadPositions.size() && !mAbort; ++i)
            {
                mTerrainViews[i]->reset();
                mWorld->preload(mTerrainViews[i], mPreloadPositions[i].mPosition, mPreloadPositions[i].mCellBounds,
                    mAbort, mLoadingReporter);
            }
            mLoadingReporter.complete();
        }

        void abort() override { mAbort = true; }

        void wait(Loading::Listener& listener) const { mLoadingReporter.wait(listener); }

    private:
        std::atomic<bool> mAbort;
        std::vector<osg::ref_ptr<Terrain::View>> mTerrainViews;
        Terrain::World* mWorld;
        std::vector<PositionCellGrid> mPreloadPositions;
        Loading::Reporter mLoadingReporter;
    };

    /// Worker thread item: update the resource system's cache, effectively deleting unused entries.
    class UpdateCacheItem : public SceneUtil::WorkItem
    {
    public:
        UpdateCacheItem(Resource::ResourceSystem* resourceSystem, double referenceTime)
            : mReferenceTime(referenceTime)
            , mResourceSystem(resourceSystem)
        {
        }

        void doWork() override { mResourceSystem->updateCache(mReferenceTime); }

    private:
        double mReferenceTime;
        Resource::ResourceSystem* mResourceSystem;
    };

    CellPreloader::CellPreloader(Resource::ResourceSystem* resourceSystem,
        Resource::BulletShapeManager* bulletShapeManager, Terrain::World* terrain, MWRender::LandManager* landManager)
        : mResourceSystem(resourceSystem)
        , mBulletShapeManager(bulletShapeManager)
        , mTerrain(terrain)
        , mLandManager(landManager)
        , mExpiryDelay(0.0)
        , mPreloadInstances(true)
        , mLastResourceCacheUpdate(0.0)
        , mLoadedTerrainTimestamp(0.0)
    {
    }

    CellPreloader::~CellPreloader()
    {
        clearAllTasks();
    }

    void CellPreloader::preload(CellStore& cell, double timestamp)
    {
        if (!mWorkQueue)
        {
            Log(Debug::Error) << "Error: can't preload, no work queue set";
            return;
        }
        if (cell.getState() == CellStore::State_Unloaded)
        {
            Log(Debug::Error) << "Error: can't preload objects for unloaded cell";
            return;
        }

        PreloadMap::iterator found = mPreloadCells.find(&cell);
        if (found != mPreloadCells.end())
        {
            // already preloaded, nothing to do other than updating the timestamp
            found->second.mTimeStamp = timestamp;
            return;
        }

        while (mPreloadCells.size() >= mMaxCacheSize)
        {
            // throw out oldest cell to make room
            PreloadMap::iterator oldestCell = mPreloadCells.begin();
            double oldestTimestamp = std::numeric_limits<double>::max();
            double threshold = 1.0; // seconds
            for (PreloadMap::iterator it = mPreloadCells.begin(); it != mPreloadCells.end(); ++it)
            {
                if (it->second.mTimeStamp < oldestTimestamp)
                {
                    oldestTimestamp = it->second.mTimeStamp;
                    oldestCell = it;
                }
            }

            if (oldestTimestamp + threshold < timestamp)
            {
                oldestCell->second.mWorkItem->abort();
                mPreloadCells.erase(oldestCell);
                ++mEvicted;
            }
            else
                return;
        }

        auto item = std::make_shared<PreloadItem>(&cell, mResourceSystem->getSceneManager(), mBulletShapeManager,
            mResourceSystem->getKeyframeManager(), mTerrain, mLandManager, mPreloadInstances);
        mWorkQueue->addWorkItem(item);

        mPreloadCells.emplace(&cell, PreloadEntry(timestamp, item));
        ++mAdded;
    }

    void CellPreloader::notifyLoaded(CellStore* cell)
    {
        PreloadMap::iterator found = mPreloadCells.find(cell);
        if (found != mPreloadCells.end())
        {
            if (found->second.mWorkItem)
            {
                found->second.mWorkItem->abort();
                found->second.mWorkItem = nullptr;
            }

            mPreloadCells.erase(found);
            ++mLoaded;
        }
    }

    void CellPreloader::clear()
    {
        for (PreloadMap::iterator it = mPreloadCells.begin(); it != mPreloadCells.end();)
        {
            if (it->second.mWorkItem)
            {
                it->second.mWorkItem->abort();
                it->second.mWorkItem = nullptr;
            }

            mPreloadCells.erase(it++);
        }
    }

    void CellPreloader::updateCache(double timestamp)
    {
        for (PreloadMap::iterator it = mPreloadCells.begin(); it != mPreloadCells.end();)
        {
            if (mPreloadCells.size() >= mMinCacheSize && it->second.mTimeStamp < timestamp - mExpiryDelay)
            {
                if (it->second.mWorkItem)
                {
                    it->second.mWorkItem->abort();
                    it->second.mWorkItem = nullptr;
                }
                mPreloadCells.erase(it++);
                ++mExpired;
            }
            else
                ++it;
        }

        if (timestamp - mLastResourceCacheUpdate > 1.0 && (!mUpdateCacheItem || mUpdateCacheItem->isDone()))
        {
            // the resource cache is cleared from the worker thread so that we're not holding up the main thread with
            // delete operations
            mUpdateCacheItem = std::make_shared<UpdateCacheItem>(mResourceSystem, timestamp);
            mWorkQueue->addWorkItem(mUpdateCacheItem, true);
            mLastResourceCacheUpdate = timestamp;
        }

        if (mTerrainPreloadItem && mTerrainPreloadItem->isDone())
        {
            mLoadedTerrainPositions = mTerrainPreloadPositions;
            mLoadedTerrainTimestamp = timestamp;
        }
    }

    void CellPreloader::setExpiryDelay(double expiryDelay)
    {
        mExpiryDelay = expiryDelay;
    }

    void CellPreloader::setPreloadInstances(bool preload)
    {
        mPreloadInstances = preload;
    }

    void CellPreloader::setWorkQueue(std::shared_ptr<SceneUtil::WorkQueue> workQueue)
    {
        mWorkQueue = workQueue;
    }

    void CellPreloader::syncTerrainLoad(Loading::Listener& listener)
    {
        if (mTerrainPreloadItem == nullptr || mTerrainPreloadItem->isDone())
            return;

        // Nothing else to do meanwhile, so more threads help.
        const unsigned int threads = getLoadingThreads();
        struct Helpers
        {
            explicit Helpers(unsigned int count) { Terrain::setPreloadHelperThreads(count); }
            ~Helpers() { Terrain::setPreloadHelperThreads(0); }
        } helpers(threads - 1);
        mTerrainPreloadItem->wait(listener);
    }

    unsigned int CellPreloader::getLoadingThreads()
    {
        const int setting = Settings::terrain().mLoadingThreads;
        if (setting > 0)
            return static_cast<unsigned int>(setting);
        return std::clamp(std::thread::hardware_concurrency(), 3u, 10u) - 2;
    }

    CellPreloader::LoadedModels CellPreloader::loadModelsNow(
        std::span<CellStore* const> cells, Loading::Listener& listener)
    {
        LoadedModels result;

        std::vector<VFS::Path::NormalizedView> meshes;
        ListModelsVisitor visitor{ meshes };
        for (CellStore* cell : cells)
            cell->forEachConst(visitor);
        std::sort(meshes.begin(), meshes.end());
        meshes.erase(std::unique(meshes.begin(), meshes.end()), meshes.end());
        if (meshes.empty())
            return result;

        Resource::SceneManager* const sceneManager = mResourceSystem->getSceneManager();
        Resource::KeyframeManager* const keyframeManager = mResourceSystem->getKeyframeManager();
        const VFS::Manager& vfs = *sceneManager->getVFS();

        std::atomic<std::size_t> next{ 0 };
        std::atomic<std::size_t> done{ 0 };
        std::mutex resultMutex;
        const auto work = [&] {
            LoadedModels loaded;
            VFS::Path::Normalized mesh;
            VFS::Path::Normalized kfname;
            for (std::size_t i; (i = next.fetch_add(1)) < meshes.size(); ++done)
            {
                // The same as PreloadItem does for one cell.
                try
                {
                    mesh = Misc::ResourceHelpers::correctMeshPath(meshes[i]);
                    mesh = Misc::ResourceHelpers::correctActorModelPath(mesh, &vfs);
                    if (!vfs.exists(mesh))
                        continue;

                    constexpr VFS::Path::ExtensionView nif("nif");
                    if (Misc::getFileName(mesh).starts_with('x') && mesh.extension() == nif)
                    {
                        kfname = mesh;
                        constexpr VFS::Path::ExtensionView kf("kf");
                        kfname.changeExtension(kf);
                        if (vfs.exists(kfname))
                            loaded.mOwnedObjects.insert(keyframeManager->get(kfname));
                    }

                    loaded.mObjects.insert(sceneManager->getTemplate(mesh));
                    if (mPreloadInstances)
                        loaded.mOwnedObjects.insert(mBulletShapeManager->cacheInstance(mesh));
                    else
                        loaded.mOwnedObjects.insert(mBulletShapeManager->getShape(mesh));
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "Failed to load mesh \"" << meshes[i] << "\": " << e.what();
                }
            }
            std::lock_guard<std::mutex> lock(resultMutex);
            result.mObjects.merge(loaded.mObjects);
            result.mOwnedObjects.merge(loaded.mOwnedObjects);
        };

        const std::size_t threads = std::min<std::size_t>(getLoadingThreads(), meshes.size());
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i)
            workers.emplace_back(work);

        // This thread keeps the loading screen going meanwhile.
        listener.setProgressRange(meshes.size());
        while (done.load() < meshes.size())
        {
            listener.setProgress(done.load());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        for (std::thread& worker : workers)
            worker.join();
        listener.setProgress(meshes.size());
        return result;
    }

    void CellPreloader::abortTerrainPreloadExcept(const PositionCellGrid* exceptPos)
    {
        if (exceptPos != nullptr && contains(mTerrainPreloadPositions, *exceptPos, Constants::CellSizeInUnits))
            return;
        if (mTerrainPreloadItem && !mTerrainPreloadItem->isDone())
        {
            mTerrainPreloadItem->abort();
            mTerrainPreloadItem->waitTillDone();
        }
        setTerrainPreloadPositions({});
    }

    void CellPreloader::setTerrainPreloadPositions(std::span<const PositionCellGrid> positions)
    {
        if (positions.empty())
        {
            mTerrainPreloadPositions.clear();
            mLoadedTerrainPositions.clear();
        }
        else if (contains(mTerrainPreloadPositions, positions, terrainPreloadMergeDistance))
            return;
        if (mTerrainPreloadItem && !mTerrainPreloadItem->isDone())
        {
            const bool sameGrids = std::ranges::equal(mTerrainPreloadPositions, positions,
                [](const PositionCellGrid& l, const PositionCellGrid& r) { return l.mCellBounds == r.mCellBounds; });
            if (!sameGrids)
            {
                mTerrainPreloadItem->abort();
                // Do not record the aborted target as loaded in updateCache.
                mTerrainPreloadPositions.clear();
            }
            return;
        }
        else
        {
            if (mTerrainViews.size() > positions.size())
                mTerrainViews.resize(positions.size());
            else if (mTerrainViews.size() < positions.size())
            {
                for (size_t i = mTerrainViews.size(); i < positions.size(); ++i)
                    mTerrainViews.emplace_back(mTerrain->createView());
            }

            mTerrainPreloadPositions.assign(positions.begin(), positions.end());
            if (!positions.empty())
            {
                mTerrainPreloadItem = std::make_shared<TerrainPreloadItem>(mTerrainViews, mTerrain, positions);
                mWorkQueue->addWorkItem(mTerrainPreloadItem);
            }
        }
    }

    bool CellPreloader::isTerrainLoaded(const PositionCellGrid& position, double referenceTime) const
    {
        return mLoadedTerrainTimestamp + mResourceSystem->getSceneManager()->getExpiryDelay() > referenceTime
            && contains(mLoadedTerrainPositions, position, Constants::CellSizeInUnits);
    }

    void CellPreloader::setTerrain(Terrain::World* terrain)
    {
        if (terrain != mTerrain)
        {
            clearAllTasks();
            mTerrain = terrain;
        }
    }

    void CellPreloader::clearAllTasks()
    {
        if (mTerrainPreloadItem)
        {
            mTerrainPreloadItem->abort();
            mTerrainPreloadItem->waitTillDone();
            mTerrainPreloadItem = nullptr;
        }

        if (mUpdateCacheItem)
        {
            mUpdateCacheItem->waitTillDone();
            mUpdateCacheItem = nullptr;
        }

        for (PreloadMap::iterator it = mPreloadCells.begin(); it != mPreloadCells.end(); ++it)
            it->second.mWorkItem->abort();

        for (PreloadMap::iterator it = mPreloadCells.begin(); it != mPreloadCells.end(); ++it)
            it->second.mWorkItem->waitTillDone();

        mPreloadCells.clear();
    }

    void CellPreloader::reportStats(unsigned int frameNumber, osg::Stats& stats) const
    {
        stats.setAttribute(frameNumber, "CellPreloader Count", static_cast<double>(mPreloadCells.size()));
        stats.setAttribute(frameNumber, "CellPreloader Added", static_cast<double>(mAdded));
        stats.setAttribute(frameNumber, "CellPreloader Evicted", static_cast<double>(mEvicted));
        stats.setAttribute(frameNumber, "CellPreloader Loaded", static_cast<double>(mLoaded));
        stats.setAttribute(frameNumber, "CellPreloader Expired", static_cast<double>(mExpired));
    }
}
