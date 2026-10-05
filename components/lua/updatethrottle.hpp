#ifndef OPENMW_COMPONENTS_LUA_UPDATETHROTTLE_H
#define OPENMW_COMPONENTS_LUA_UPDATETHROTTLE_H

#include <cstdint>
#include <optional>

namespace LuaUtil
{
    /// Runs a script container's onUpdate every `interval` frames instead of every frame while it's allowed to
    /// ([Lua] distant update interval): the frame time of the skipped frames is added up and passed on, so scripts
    /// that integrate over dt still see the same total time. Containers use different phases, so 1/interval of
    /// them update each frame and the cost is spread evenly.
    class UpdateThrottle
    {
    public:
        /// The dt to pass to onUpdate this frame, or nothing to skip it.
        /// @param fullRate this container must update every frame (near the player, in combat, ...)
        std::optional<float> next(float dt, bool fullRate, std::uint64_t frame, unsigned phase, unsigned interval)
        {
            if (fullRate || interval <= 1)
            {
                const float total = dt + mSkippedTime;
                mSkippedTime = 0;
                return total;
            }
            mSkippedTime += dt;
            if ((frame + phase) % interval != 0)
                return std::nullopt;
            const float total = mSkippedTime;
            mSkippedTime = 0;
            return total;
        }

        float skippedTime() const { return mSkippedTime; }

    private:
        float mSkippedTime = 0;
    };
}

#endif
