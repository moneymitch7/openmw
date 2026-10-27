#include "worker.hpp"

#include <chrono>

#include "luamanagerimp.hpp"

#include "apps/openmw/profile.hpp"

#include <components/debug/debuglog.hpp>
#include <components/settings/values.hpp>

namespace MWLua
{
    namespace
    {
        // Step size (roughly KBytes of GC work) per background-loop lua_gc call.
        // Small enough that finishGc() never waits long for the step in flight.
        constexpr int sGcStepSize = 10;
    }

    Worker::Worker(LuaManager& manager)
        : mManager(manager)
    {
        if (Settings::lua().mLuaNumThreads > 0)
            mThread = std::thread([this] { run(); });
    }

    Worker::~Worker()
    {
        if (mThread && mThread->joinable())
        {
            Log(Debug::Error)
                << "Unexpected destruction of LuaWorker; likely there is an unhandled exception in the main thread.";
            join();
        }
    }

    void Worker::allowUpdate(Misc::FrameClock::time_point statsEpoch, unsigned frameNumber, Misc::FrameStats& stats)
    {
        if (!mThread)
            return;
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mUpdateRequest = UpdateRequest{ .mStatsEpoch = statsEpoch, .mFrameNumber = frameNumber, .mStats = &stats };
        }
        mCV.notify_one();
    }

    void Worker::finishUpdate(Misc::FrameClock::time_point statsEpoch, unsigned frameNumber, Misc::FrameStats& stats)
    {
        if (mThread)
        {
            std::unique_lock<std::mutex> lk(mMutex);
            mCV.wait(lk, [&] { return !mUpdateRequest.has_value(); });
        }
        else
            update(statsEpoch, frameNumber, stats);
    }

    void Worker::gc(bool relaxed)
    {
        const int steps = Settings::lua().mGcStepsPerFrame;
        if (steps <= 0)
            return;
        if (!mThread)
        {
            mManager.gcStep(steps, relaxed);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mGcRequest = true;
            mGcRelaxed = relaxed;
        }
        mCV.notify_one();
    }

    void Worker::finishGc()
    {
        if (!mThread)
            return;
        const auto start = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lk(mMutex);
        mGcRequest = false;
        mCV.wait(lk, [&] { return !mGcInProgress; });
        lk.unlock();
        // OpenMGE XE: the frame had to wait for a garbage collection step still running
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (ms >= 10.0)
            Log(Debug::Info) << "Frame waited " << static_cast<int>(ms) << " ms for Lua garbage collection";
    }

    void Worker::join()
    {
        if (mThread)
        {
            {
                std::lock_guard<std::mutex> lk(mMutex);
                mJoinRequest = true;
            }
            mCV.notify_one();
            mThread->join();
        }
    }

    void Worker::update(Misc::FrameClock::time_point statsEpoch, unsigned frameNumber, Misc::FrameStats& stats)
    {
        OMW::ScopedProfile<OMW::UserStatsType::Lua> profile(statsEpoch, frameNumber, stats);

        mManager.update();
    }

    void Worker::run() noexcept
    {
        while (true)
        {
            std::unique_lock<std::mutex> lk(mMutex);
            mCV.wait(lk, [&] { return mUpdateRequest.has_value() || mGcRequest || mJoinRequest; });
            if (mJoinRequest)
                break;

            if (!mUpdateRequest.has_value())
            {
                // GC step with the lock dropped: finishGc() can post its stop request
                // while the step runs. The mutex guards only the flags.
                mGcInProgress = true;
                const bool relaxed = mGcRelaxed;
                lk.unlock();
                bool cycleFinished = false;
                try
                {
                    cycleFinished = mManager.gcStep(sGcStepSize, relaxed);
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Error) << "Failed to run Lua GC step: " << e.what();
                    cycleFinished = true;
                }
                lk.lock();
                mGcInProgress = false;
                if (cycleFinished)
                    mGcRequest = false;
                if (!mGcRequest)
                {
                    lk.unlock();
                    mCV.notify_one();
                }
                continue;
            }

            try
            {
                update(mUpdateRequest->mStatsEpoch, mUpdateRequest->mFrameNumber, *mUpdateRequest->mStats);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Error) << "Failed to update LuaManager: " << e.what();
            }

            mUpdateRequest.reset();
            // OpenMGE XE [Lua] gc while drawing: the scripts are done for this frame and the main thread doesn't use
            // Lua until the frame is drawn (it ran alongside the drawing): collect from now on, not only once the
            // frame is drawn. The next frame's start stops it as before (finishGc).
            if (Settings::lua().mGcWhileDrawing && Settings::lua().mGcStepsPerFrame > 0)
                mGcRequest = true;
            lk.unlock();
            mCV.notify_one();
        }
    }
}
