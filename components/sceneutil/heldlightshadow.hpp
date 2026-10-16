#ifndef OPENMW_COMPONENTS_SCENEUTIL_HELDLIGHTSHADOW_H
#define OPENMW_COMPONENTS_SCENEUTIL_HELDLIGHTSHADOW_H

#include <array>
#include <cstddef>

#include <osg/Callback>
#include <osg/Camera>
#include <osg/StateSet>
#include <osg/TextureCubeMap>
#include <osg/Uniform>
#include <osg/Vec3f>
#include <osg/observer_ptr>

namespace osgUtil
{
    class CullVisitor;
}

namespace Shader
{
    class ShaderManager;
}

namespace SceneUtil
{
    class LightManager;
    class LightSource;

    /// OpenMGE XE lantern shadows: the lights the player carries (a torch, a worn lantern's stand-in light) cast real
    /// shadows. Each frame the scene around each of them is drawn into the six faces of a depth cube map, from the
    /// light, and the lighting shaders darken what the light can't see (lib/light/util.glsl, heldLightShadow). Per
    /// pixel: a table's edge, a pillar or a companion throws a shadow across the floor, and a wall keeps the light from
    /// the next room. Only those lights; the others are as they were.
    class HeldLightShadow
    {
    public:
        /// As many of the player's lights as cast shadows at once (a torch in hand and a lantern on the belt).
        static constexpr std::size_t sMaxLights = 2;

        /// @param castMask what casts shadows (node masks)
        /// @param textureUnit the first of sMaxLights texture units, one cube map each
        HeldLightShadow(Shader::ShaderManager& shaderManager, int textureUnit, unsigned int castMask, int resolution);
        ~HeldLightShadow();

        /// Draws the shadows of the lights, binds the cube maps to @a receivers (the root state set of what is lit)
        /// and adds the culling to @a lightManager, the root of the lit scene.
        void install(LightManager* lightManager, osg::StateSet* receivers);

        /// Call every frame: where the player is (the lights carried by the player, or carried by no one within
        /// reach of this point, cast the shadows), and whether shadows are wanted at all now.
        void setFocus(const osg::Vec3f& position, bool enabled);

        void cull(LightManager* lightManager, osgUtil::CullVisitor* cv);

    private:
        void cullFaces(LightManager* lightManager, osgUtil::CullVisitor* cv);

        struct Slot
        {
            osg::ref_ptr<osg::TextureCubeMap> mTexture;
            std::array<osg::ref_ptr<osg::Camera>, 6> mCameras;
            osg::observer_ptr<LightSource> mCaster;
        };

        std::array<Slot, sMaxLights> mSlots;
        osg::ref_ptr<osg::StateSet> mCastingStateSet;
        osg::ref_ptr<osg::StateSet> mBinStateSet;
        // double buffered by frame: the draw of the previous frame may still read the other
        std::array<osg::ref_ptr<osg::StateSet>, 2> mReceiverStateSets;
        std::array<osg::ref_ptr<osg::Uniform>, 2> mPositionUniforms;
        osg::ref_ptr<osg::StateSet> mInactiveStateSet;
        LightManager* mLightManager = nullptr;
        osg::observer_ptr<LightManager> mInstalledOn;
        osg::ref_ptr<osg::Callback> mCullCallback;
        unsigned int mCastMask;
        int mResolution;
        int mTextureUnit;
        osg::Vec3f mFocus;
        bool mEnabled = false;
        bool mInFaces = false;
        std::size_t mLastFrame = ~std::size_t(0);
        bool mActive = false;
    };
}

#endif
