#include <gtest/gtest.h>

#include <components/lua/updatethrottle.hpp>

namespace
{
    TEST(LuaUpdateThrottleTest, FullRateUpdatesEveryFrame)
    {
        LuaUtil::UpdateThrottle throttle;
        for (std::uint64_t frame = 0; frame < 10; ++frame)
        {
            const std::optional<float> dt = throttle.next(0.016f, true, frame, 0, 4);
            ASSERT_TRUE(dt.has_value());
            EXPECT_FLOAT_EQ(*dt, 0.016f);
        }
    }

    TEST(LuaUpdateThrottleTest, IntervalOneIsEveryFrame)
    {
        LuaUtil::UpdateThrottle throttle;
        for (std::uint64_t frame = 0; frame < 10; ++frame)
            EXPECT_TRUE(throttle.next(0.016f, false, frame, 3, 1).has_value());
    }

    TEST(LuaUpdateThrottleTest, DistantUpdatesEveryIntervalFramesWithSummedTime)
    {
        LuaUtil::UpdateThrottle throttle;
        int calls = 0;
        float total = 0;
        for (std::uint64_t frame = 1; frame <= 12; ++frame)
        {
            if (const std::optional<float> dt = throttle.next(0.01f, false, frame, 0, 3))
            {
                EXPECT_EQ(frame % 3, 0u);
                ++calls;
                total += *dt;
            }
        }
        EXPECT_EQ(calls, 4);
        EXPECT_NEAR(total, 0.12f, 1e-5f); // no time lost
    }

    TEST(LuaUpdateThrottleTest, PhasesSpreadContainersOverFrames)
    {
        // with interval 3, containers with phases 0, 1 and 2 update on different frames
        for (unsigned phase = 0; phase < 3; ++phase)
        {
            LuaUtil::UpdateThrottle throttle;
            std::uint64_t first = 0;
            for (std::uint64_t frame = 1; frame <= 3 && first == 0; ++frame)
                if (throttle.next(0.01f, false, frame, phase, 3))
                    first = frame;
            EXPECT_EQ((first + phase) % 3, 0u);
        }
    }

    TEST(LuaUpdateThrottleTest, ComingNearFlushesSkippedTime)
    {
        LuaUtil::UpdateThrottle throttle;
        EXPECT_FALSE(throttle.next(0.01f, false, 1, 0, 4).has_value());
        EXPECT_FALSE(throttle.next(0.01f, false, 2, 0, 4).has_value());
        const std::optional<float> dt = throttle.next(0.01f, true, 3, 0, 4);
        ASSERT_TRUE(dt.has_value());
        EXPECT_NEAR(*dt, 0.03f, 1e-6f);
        EXPECT_EQ(throttle.skippedTime(), 0.f);
    }
}
