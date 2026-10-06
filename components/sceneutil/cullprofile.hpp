#ifndef OPENMW_COMPONENTS_SCENEUTIL_CULLPROFILE_H
#define OPENMW_COMPONENTS_SCENEUTIL_CULLPROFILE_H

#include <array>
#include <cstddef>

#include <osg/Timer>

namespace SceneUtil::CullProfile
{
    /// Where the cull traversal spends its time, for the F3 profiler and the performance log.
    ///
    /// The viewer culls on the main thread (it draws on its own thread), so this is plain state that the cull code adds
    /// to and the engine takes once a frame, after the cull. Main thread only.
    enum class Section : std::size_t
    {
        /// Shadow maps: the casters' light-space extent and their cull for each cascade (the main view's own
        /// traversal is not included).
        Shadows,
        /// The casters' light-space extent alone, part of Shadows.
        ShadowBounds,
        /// The water reflection camera.
        Water,
        /// Occluders drawn into the occlusion buffer before the main view is culled.
        Occluders,
        /// Choosing the point lights of each object.
        LightLists,
        Count
    };

    struct Totals
    {
        osg::Timer_t mFirst = 0;
        osg::Timer_t mLast = 0;
        osg::Timer_t mTaken = 0;
        unsigned int mCalls = 0;
    };

    using AllTotals = std::array<Totals, static_cast<std::size_t>(Section::Count)>;

    namespace Detail
    {
        inline bool sEnabled = false;
        inline AllTotals sTotals{};
    }

    /// Whether timings are collected: the profiler or the performance log is on.
    inline bool isEnabled()
    {
        return Detail::sEnabled;
    }

    inline void setEnabled(bool enabled)
    {
        Detail::sEnabled = enabled;
    }

    inline void add(Section section, osg::Timer_t start, osg::Timer_t end)
    {
        Totals& totals = Detail::sTotals[static_cast<std::size_t>(section)];
        if (totals.mCalls == 0)
            totals.mFirst = start;
        totals.mLast = end;
        totals.mTaken += end - start;
        ++totals.mCalls;
    }

    /// The totals added since the last call, which clears them.
    inline AllTotals take()
    {
        AllTotals result = Detail::sTotals;
        Detail::sTotals = AllTotals{};
        return result;
    }

    /// Times its own lifetime into a section while timings are collected. Section::Count times nothing.
    class Scope
    {
    public:
        explicit Scope(Section section)
            : mSection(section)
            , mStart(isEnabled() && section != Section::Count ? osg::Timer::instance()->tick() : 0)
        {
        }

        ~Scope()
        {
            if (mStart != 0)
                add(mSection, mStart, osg::Timer::instance()->tick());
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        const Section mSection;
        const osg::Timer_t mStart;
    };
}

#endif
