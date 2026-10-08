#ifndef OPENMW_COMPONENTS_SCENEUTIL_LOADPROFILE_H
#define OPENMW_COMPONENTS_SCENEUTIL_LOADPROFILE_H

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace SceneUtil::LoadProfile
{
    /// Where building the distant land (terrain, paged objects, groundcover) spends its time, for the slow cell change
    /// log. Chunks are built on several threads at once, so these are totals over all of them: they add up to more than
    /// the time that passed. Read them before and after a load and take the difference.
    enum class Step : std::size_t
    {
        /// Terrain chunks: height meshes, blend maps and their textures.
        Terrain,
        /// Groundcover chunks.
        Groundcover,
        /// Paged object chunks, all of it (the steps below are parts of it).
        Objects,
        /// Reading the cells' references from the content files.
        ObjectRefs,
        /// Loading the models (and with them their textures), and finding their _dist versions.
        ObjectModels,
        /// Placing the instances (copying the models' node graphs).
        ObjectCopies,
        /// Building the occlusion culling meshes of large objects.
        ObjectOccluders,
        /// Merging the instances' meshes.
        ObjectMerging,
        /// Paged object chunks built (a count, not a time).
        ObjectChunks,
        Count
    };

    namespace Detail
    {
        inline std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Step::Count)> sTotals{};
    }

    inline void add(Step step, std::uint64_t value)
    {
        Detail::sTotals[static_cast<std::size_t>(step)].fetch_add(value, std::memory_order_relaxed);
    }

    /// Microseconds (or the count for ObjectChunks) so far, on all threads together.
    inline std::uint64_t get(Step step)
    {
        return Detail::sTotals[static_cast<std::size_t>(step)].load(std::memory_order_relaxed);
    }

    using Snapshot = std::array<std::uint64_t, static_cast<std::size_t>(Step::Count)>;

    inline Snapshot snapshot()
    {
        Snapshot result{};
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = Detail::sTotals[i].load(std::memory_order_relaxed);
        return result;
    }

    /// Adds the time from its construction to its destruction to a step.
    class Scope
    {
    public:
        explicit Scope(Step step)
            : mStep(step)
            , mStart(std::chrono::steady_clock::now())
        {
        }

        ~Scope()
        {
            add(mStep,
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - mStart)
                        .count()));
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Step mStep;
        std::chrono::steady_clock::time_point mStart;
    };
}

#endif
