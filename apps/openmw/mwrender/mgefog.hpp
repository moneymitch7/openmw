#ifndef OPENMW_MWRENDER_MGEFOG_H
#define OPENMW_MWRENDER_MGEFOG_H

#include <components/sceneutil/stateupdater.hpp>

#include <osg/Vec3f>

namespace MWRender
{
    // [Fog] mge fog: the weather as the MGE XE fog shaders (lib/fog/mge.glsl) take it, from the weather manager.
    struct MgeFogWeather
    {
        bool mExterior = false;
        // how much of the weather is Clear or Cloudy (MGE's nice weathers), squared as MGE XE does
        float mNiceWeather = 0.f;
        osg::Vec3f mSkyColor{ 0.5f, 0.5f, 0.5f };
        // the fog colour; through a transition between a nice weather and a bad one, the bad weather's own
        osg::Vec3f mFogColor{ 0.5f, 0.5f, 0.5f };
        // the weather's distant land fog factor and offset (0-1), after the fog density settings
        float mFogFactor = 1.f;
        float mFogOffset = 0.f;
        // the same for the weather it is turning into, and how far it has turned (0-1)
        float mNextFogFactor = 1.f;
        float mNextFogOffset = 0.f;
        float mTransition = 0.f;
        bool mNiceStormTransition = false;
    };

    // The scene root's state updater with the MGE fog's uniforms too. Everything that is the same for the whole
    // frame is worked out here once per frame (the fog range for the weather and its transition, MGE XE's near-range
    // fit), leaving the shaders only the per-pixel part. Only made with the setting on.
    class MgeFogStateUpdater : public SceneUtil::StateUpdater
    {
    public:
        void setDefaults(osg::StateSet* stateset) override;
        void apply(osg::StateSet* stateset, osg::NodeVisitor* nv) override;

        void setWeather(const MgeFogWeather& weather) { mWeather = weather; }
        void setNight(bool night) { mNight = night; }
        // where the sunlight comes from, world space
        void setSunDirection(const osg::Vec3f& direction);
        void setViewDistance(float distance) { mViewDistance = distance; }
        void setViewerUnderwater(bool underwater) { mViewerUnderwater = underwater; }

    private:
        MgeFogWeather mWeather;
        osg::Vec3f mSunDirection{ 0.f, 0.f, 1.f };
        bool mNight = false;
        float mViewDistance = 0.f;
        bool mViewerUnderwater = false;
    };
}

#endif
