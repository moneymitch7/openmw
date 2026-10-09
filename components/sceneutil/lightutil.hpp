#ifndef OPENMW_COMPONENTS_LIGHTUTIL_H
#define OPENMW_COMPONENTS_LIGHTUTIL_H

#include <osg/Vec4f>
#include <osg/ref_ptr>

namespace osg
{
    class Group;
    class Light;
}

namespace ESM
{
    struct Light;
}

namespace SceneUtil
{
    class Light;
    class LightSource;
    struct LightCommon;

    /// Adjustments to one light on top of its game data, for lights a player wants to tune on their own (the torch they
    /// carry). The defaults leave the light as the game data has it.
    struct LightTuning
    {
        /// Scales the light's colour.
        float mBrightness = 1.f;
        /// Scales the radius the light reaches (where it ends and which objects it lights), but not how bright it is
        /// at a given distance.
        float mReach = 1.f;
        /// Flattens the light around the distance where it reaches its full colour (a third of its radius with the
        /// usual attenuation settings): 0 leaves it, higher values dim the bright spot close to the light and brighten
        /// it further out. 1 caps the brightest point at about 4/3 of the full colour.
        float mSoftness = 0.f;
        /// The light's own light bounce (see [Shaders] light bounce) instead of the scene's; negative uses the
        /// scene's. Only with clustered lighting.
        float mBounce = -1.f;
        /// How strongly a steady light (one its record doesn't make flicker or pulse) wavers like a flame, 0 to 1;
        /// 0 keeps it steady.
        float mFlameFlicker = 0.f;

        bool isDefault() const
        {
            return mBrightness == 1.f && mReach == 1.f && mSoftness == 0.f && mBounce < 0.f && mFlameFlicker == 0.f;
        }
    };

    /// @brief Set up global attenuation settings for a Light.
    /// @param radius The radius of the light source.
    /// @param isExterior Is the light outside? May be used for deciding which attenuation settings to use.
    void configureLight(Light* light, float radius, bool isExterior);

    /// @brief Convert an ESM::Light to a SceneUtil::LightSource, and add it to a sub graph.
    /// @note If the sub graph contains a node named "AttachLight" (case insensitive), then the light is added to that.
    /// Otherwise, the light is attached directly to the root node of the subgraph.
    /// @param node The sub graph to add a light to
    /// @param esmLight The light definition coming from the game files containing radius, color, flicker, etc.
    /// @param lightMask Mask to assign to the newly created LightSource.
    /// @param isExterior Is the light outside? May be used for deciding which attenuation settings to use.
    osg::ref_ptr<LightSource> addLight(osg::Group* node, const SceneUtil::LightCommon& esmLight, unsigned int lightMask,
        bool isExterior, const LightTuning& tuning = {});

    /// @brief Convert an ESM::Light to a SceneUtil::LightSource, and return it.
    /// @param esmLight The light definition coming from the game files containing radius, color, flicker, etc.
    /// @param lightMask Mask to assign to the newly created LightSource.
    /// @param isExterior Is the light outside? May be used for deciding which attenuation settings to use.
    /// @param ambient Ambient component of the light.
    osg::ref_ptr<LightSource> createLightSource(const SceneUtil::LightCommon& esmLight, unsigned int lightMask,
        bool isExterior, const osg::Vec4f& ambient = osg::Vec4f(0, 0, 0, 1), const LightTuning& tuning = {});

    /// @brief Set the radius, attenuation and colour of a light made by createLightSource again, from its game data
    /// and a new tuning. The light keeps its identity, flicker and ambient.
    void retuneLightSource(
        LightSource& lightSource, const SceneUtil::LightCommon& esmLight, bool isExterior, const LightTuning& tuning);
}

#endif
