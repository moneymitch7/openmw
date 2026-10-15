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

    /// OpenMGE XE lantern shadows: the light the player carries (a torch, a worn lantern's stand-in light) casts real
    /// shadows. Each frame the scene around it is drawn into the six faces of a depth cube map, from the light, and the
    /// lighting shaders darken what the light can't see (lib/light/util.glsl, heldLightShadow). Per pixel: a table's
    /// edge, a pillar or a companion throws a shadow across the floor, and a wall keeps the light from the next room.
    /// Only that one light; the others are as they were.
    class HeldLightShadow
    {
    public:
        /// @param castMask what casts shadows (node masks)
        HeldLightShadow(Shader::ShaderManager& shaderManager, int textureUnit, unsigned int castMask, int resolution);
        ~HeldLightShadow();

        /// Draws the shadows of the light, binds the cube map to @a receivers (the root state set of what is lit) and
        /// adds the culling to @a lightManager, the root of the lit scene.
        void install(LightManager* lightManager, osg::StateSet* receivers);

        /// Call every frame: where the player is (the light carried within reach of this point casts the shadows),
        /// and whether shadows are wanted at all now.
        void setFocus(const osg::Vec3f& position, bool enabled);

        void cull(LightManager* lightManager, osgUtil::CullVisitor* cv);

    private:
        void cullFaces(LightManager* lightManager, osgUtil::CullVisitor* cv);

        osg::ref_ptr<osg::TextureCubeMap> mTexture;
        std::array<osg::ref_ptr<osg::Camera>, 6> mCameras;
        osg::ref_ptr<osg::StateSet> mCastingStateSet;
        osg::ref_ptr<osg::StateSet> mBinStateSet;
        // double buffered by frame: the draw of the previous frame may still read the other
        std::array<osg::ref_ptr<osg::StateSet>, 2> mReceiverStateSets;
        std::array<osg::ref_ptr<osg::Uniform>, 2> mPositionUniforms;
        std::array<osg::ref_ptr<osg::Uniform>, 2> mRangeUniforms;
        osg::ref_ptr<osg::StateSet> mInactiveStateSet;
        osg::observer_ptr<LightSource> mCaster;
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
