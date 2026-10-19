#ifndef OPENMW_COMPONENTS_SCENEUTIL_LAMPSHADOWS_H
#define OPENMW_COMPONENTS_SCENEUTIL_LAMPSHADOWS_H

#include <array>
#include <cstddef>
#include <vector>

#include <osg/BoundingSphere>
#include <osg/Callback>
#include <osg/Camera>
#include <osg/StateSet>
#include <osg/Texture2DArray>
#include <osg/Quat>
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
    class PositionAttitudeTransform;

    /// Tag on the base node of something drawn into lamps' still shadows that can move (a door turning open or shut):
    /// the lamps around it draw their still shadows again when it does.
    struct LampShadowMovable
    {
    };

    /// OpenMGE XE lamp shadows: the lamps placed in a room (wall sconces, candles, braziers, ...) cast real shadows,
    /// as the player's own lights do (HeldLightShadow). A lamp doesn't move, so what doesn't move either (walls,
    /// floors, furniture) is drawn from it once, into the six faces of a depth "cube" (six layers of a texture array),
    /// when it comes into the room, and kept. Characters move: the lamps nearest the player also draw them, every frame,
    /// into faces of their own. The lighting shaders look the lamp's faces up (lib/light/util.glsl, lampShadow) through
    /// the light's own data (PointLight::mCarrierFoot), so any number of lamps cost the same per pixel as one.
    class LampShadows
    {
    public:
        /// How many lamps keep their still shadows at once (the nearest ones).
        static constexpr std::size_t sMaxLamps = 32;
        /// How many of them also draw the characters around them each frame.
        static constexpr std::size_t sLiveLamps = 4;
        /// Faces start this close to the lamp.
        static constexpr float sNearPlane = 4.f;

        /// @param textureUnit the first of two texture units (still shadows, then live ones)
        /// @param stillMask what casts the still shadows (node masks), @param liveMask what casts the live ones
        LampShadows(Shader::ShaderManager& shaderManager, int textureUnit, unsigned int stillMask,
            unsigned int liveMask, int resolution);
        ~LampShadows();

        /// Binds the face textures to @a receivers (the root state set of what is lit) and adds the culling to
        /// @a lightManager, the root of the lit scene.
        void install(LightManager* lightManager, osg::StateSet* receivers);

        /// Call every frame: where the player is (the nearest lamps get shadows), and whether shadows are wanted now.
        void setFocus(const osg::Vec3f& position, bool enabled);

        void cull(LightManager* lightManager, osgUtil::CullVisitor* cv);

    private:
        struct Slot
        {
            std::array<osg::ref_ptr<osg::Camera>, 6> mCameras;
            osg::observer_ptr<LightSource> mLight;
            osg::Vec3f mPosition;
            float mFar = 0.f;
            // the frame the faces were (first) drawn in, and the frame the slot was let go in
            std::size_t mDrawnFrame = 0;
            std::size_t mFreedFrame = 0;
            bool mDrawn = false;
            bool mPending = false;
            int mMoves = 0;
            // live faces: which held characters when last drawn (those are drawn again, to clear them)
            unsigned int mFacesWithCasters = 0;
        };

        void cullLamps(LightManager* lightManager, osgUtil::CullVisitor* cv);
        /// Finds the characters (mLiveCasters) and the movables, and has the lamps around a movable that moved draw
        /// their still faces again.
        void findCasters(LightManager* lightManager);
        void drawFaces(Slot& slot, osgUtil::CullVisitor* cv, unsigned int mask, unsigned int faces = 0x3f);
        /// Which of the slot's faces see any of @a casters (bit per face).
        unsigned int facesSeeing(const Slot& slot, const std::vector<osg::BoundingSphere>& casters) const;
        void release(Slot& slot, std::size_t frame);
        void releaseAll(std::size_t frame);

        std::vector<Slot> mStill;
        std::vector<Slot> mLive;
        osg::ref_ptr<osg::Texture2DArray> mStillTexture;
        osg::ref_ptr<osg::Texture2DArray> mLiveTexture;
        osg::ref_ptr<osg::StateSet> mCastingStateSet;
        osg::ref_ptr<osg::StateSet> mBinStateSet;
        osg::observer_ptr<LightManager> mInstalledOn;
        osg::ref_ptr<osg::Callback> mCullCallback;
        // lamps found to move (on a moving platform, a spell's light): left without shadows
        std::vector<osg::observer_ptr<LightSource>> mMoving;
        // the characters this frame (what the live faces draw) and their bounds
        std::vector<osg::ref_ptr<osg::Node>> mLiveCasters;
        std::vector<osg::BoundingSphere> mLiveCasterBounds;
        // the movables and where they were
        struct Movable
        {
            osg::observer_ptr<PositionAttitudeTransform> mNode;
            osg::Vec3f mPosition;
            osg::Quat mAttitude;
            bool mSeen = false;
        };
        std::vector<Movable> mMovables;
        unsigned int mStillMask;
        unsigned int mLiveMask;
        int mResolution;
        int mTextureUnit;
        float mTanHalfFov;
        osg::Vec3f mFocus;
        bool mEnabled = false;
        bool mInFaces = false;
        std::size_t mLastFrame = ~std::size_t(0);
        std::size_t mLoggedCount = ~std::size_t(0);
    };
}

#endif
