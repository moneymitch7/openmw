#ifndef OPENMW_MWRENDER_WATER_H
#define OPENMW_MWRENDER_WATER_H

#include <memory>
#include <vector>

#include <osg/Vec2f>
#include <osg/Vec3d>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/esm/refid.hpp>

#include <components/settings/settings.hpp>
#include <components/vfs/pathutil.hpp>

namespace osg
{
    class Group;
    class PositionAttitudeTransform;
    class Drawable;
    class Geometry;
    class Node;
    class Callback;
    class Image;
    class Texture2D;
}

namespace ESMTerrain
{
    class Storage;
}

namespace osgUtil
{
    class IncrementalCompileOperation;
}

namespace Resource
{
    class ResourceSystem;
}

namespace MWWorld
{
    class CellStore;
    class Ptr;
}

namespace Fallback
{
    class Map;
}

namespace MWRender
{

    class Reflection;
    class RippleSimulation;
    class RainSettingsUpdater;
    class Ripples;

    /// Water rendering
    class Water
    {
        osg::ref_ptr<RainSettingsUpdater> mRainSettingsUpdater;

        osg::ref_ptr<osg::Group> mParent;
        osg::ref_ptr<osg::Group> mSceneRoot;
        osg::ref_ptr<osg::PositionAttitudeTransform> mWaterNode;
        osg::ref_ptr<osg::Geometry> mWaterGeom;
        Resource::ResourceSystem* mResourceSystem;
        osg::ref_ptr<osgUtil::IncrementalCompileOperation> mIncrementalCompileOperation;

        std::unique_ptr<RippleSimulation> mSimulation;

        osg::ref_ptr<Reflection> mReflection;
        osg::ref_ptr<Ripples> mRipples;

        bool mEnabled;
        bool mToggled;
        float mTop;
        bool mInterior;
        bool mShowWorld;

        osg::Callback* mCullCallback;
        osg::ref_ptr<osg::Callback> mShaderWaterStateSetUpdater;

        // OpenMGE XE 3D water ([Water] wave height): a dense grid around the camera displaced by the water shader,
        // the big flat plane is cut out under it. Waves flatten in shallow water (from a map of the water depth
        // over the terrain around the player), in interiors and at the grid's edge.
        osg::ref_ptr<osg::Geometry> mWaveGrid;
        osg::ref_ptr<osg::Image> mWaveDepthImage;
        osg::ref_ptr<osg::Texture2D> mWaveDepthMap;
        osg::Vec2f mWaveDepthMapOrigin; // world xy of the map's corner
        float mWaveDepthMapWaterLevel = 0.f;
        bool mWaveDepthMapValid = false;
        ESMTerrain::Storage* mTerrainStorage = nullptr;
        ESM::RefId mWorldspace;
        float mWaveHeight = 0.f; // setting: peak height in the windiest weather, 0 = flat water
        float mWaveAmplitude = 0.f; // current peak height (weather-scaled)

        void createWaveGrid();
        void rebuildWaveDepthMap(const osg::Vec2f& center);

        osg::Vec3f getSceneNodeCoordinates(int gridX, int gridY);
        void updateVisible();

        void createSimpleWaterStateSet(osg::Node* node, float alpha);

        void createShaderWaterStateSet(osg::Node* node);

        void updateWaterMaterial();

    public:
        Water(osg::Group* parent, osg::Group* sceneRoot, Resource::ResourceSystem* resourceSystem,
            osgUtil::IncrementalCompileOperation* ico);
        ~Water();

        void setCullCallback(osg::Callback* callback);

        void listAssetsToPreload(std::vector<VFS::Path::Normalized>& textures);

        void setEnabled(bool enabled);

        bool toggle();

        bool isVisible() const { return mEnabled && mToggled; }

        bool isUnderwater(const osg::Vec3f& pos) const;

        /// adds an emitter, position will be tracked automatically using its scene node
        void addEmitter(const MWWorld::Ptr& ptr, float scale = 1.f, float force = 1.f);
        void removeEmitter(const MWWorld::Ptr& ptr);
        void updateEmitterPtr(const MWWorld::Ptr& old, const MWWorld::Ptr& ptr);
        void emitRipple(const osg::Vec3f& pos);

        void removeCell(const MWWorld::CellStore* store); ///< remove all emitters in this cell

        void clearRipples();

        void changeCell(const MWWorld::CellStore* store);
        void setHeight(const float height);
        void setRainIntensity(const float rainIntensity);

        void update(float dt, bool paused);

        /// 3D water: weather (base wind speed, 0..1) and the player's position, once per frame.
        void updateWaves(const osg::Vec3f& playerPos, float windSpeed);
        void setTerrainStorage(ESMTerrain::Storage* storage) { mTerrainStorage = storage; }

        /// For the water shader's state set updater.
        float getWaveAmplitude() const { return mWaveAmplitude; }
        osg::Vec2f getWaveDepthMapOrigin() const { return mWaveDepthMapOrigin; }
        osg::Texture2D* getWaveDepthMap() const { return mWaveDepthMap.get(); }

        osg::Vec3d getPosition() const;

        float getHeight() const { return mTop; }

        osg::Drawable* getDrawable() const;

        void processChangedSettings(const Settings::CategorySettingVector& settings);

        void showWorld(bool show);
    };

}

#endif
