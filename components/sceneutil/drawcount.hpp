#ifndef OPENMW_COMPONENTS_SCENEUTIL_DRAWCOUNT_H
#define OPENMW_COMPONENTS_SCENEUTIL_DRAWCOUNT_H

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace SceneUtil::DrawCount
{
    /// OpenMGE XE: what the draws of a frame are, for the performance log. Counted on one frame in ten by the
    /// cull visitor (see installCountingCullVisitor), from the scene graph path of each drawable it keeps.
    enum class Category : std::size_t
    {
        Land, // the terrain
        DistantObjects, // object paging chunks outside the cells around the player
        NearbyMerged, // object paging chunks of the cells around the player (merged statics)
        Objects, // objects drawn on their own: clutter, containers, doors, lights, unmerged statics
        Actors, // NPCs, creatures, the player
        Grass, // groundcover
        Other, // sky, water surface, effects, particles
        ShadowMaps, // everything drawn into the sun's shadow maps
        Reflection, // everything drawn into the water reflection
        OtherPasses, // any other render to texture (maps, previews)
        Count
    };

    constexpr std::size_t sNumCategories = static_cast<std::size_t>(Category::Count);

    /// The names object paging gives its chunks, which tell nearby merged statics from distant ones.
    constexpr std::string_view sActiveGridChunkName = "ObjectPagingActiveGrid";
    constexpr std::string_view sDistantChunkName = "ObjectPagingDistant";

    /// The stats attribute each category is reported under.
    std::string_view getStatName(Category category);

    /// Makes the cull visitors created from now on count draws when enabled. Call before the viewer is made.
    void installCountingCullVisitor(bool enabled);

    /// The draws of the most recently counted frame, and whether one has been counted.
    bool getLatest(std::array<unsigned int, sNumCategories>& counts);

    /// Whether draws are counted (the performance log is on).
    bool isEnabled();

    /// The name an object's own node gets so that its draws in the main view are told apart in the log:
    /// sObjectLabelPrefix, then why it is not merged, the kind of object, its id and what tells this placed copy from
    /// others, separated by '|'.
    constexpr std::string_view sObjectLabelPrefix = "draws|";
    std::string makeObjectLabel(
        std::string_view reason, std::string_view kind, std::string_view id, std::string_view instance);

    /// Writes to the log, at most every 15 seconds, what the objects drawn on their own in the main view were in the
    /// most recently counted frame. Call once a frame.
    void logObjects();
}

#endif
