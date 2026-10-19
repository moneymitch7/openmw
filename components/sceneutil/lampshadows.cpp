#include "lampshadows.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <osg/ClipControl>
#include <osg/CullFace>
#include <osg/Depth>
#include <osg/Image>
#include <osg/PolygonOffset>
#include <osg/Polytope>
#include <osg/Texture2D>
#include <osgUtil/CullVisitor>

#include <components/debug/debuglog.hpp>
#include <components/misc/constants.hpp>
#include <components/shader/shadermanager.hpp>

#include "glextensions.hpp"
#include "lightmanager.hpp"
#include "shadowsbin.hpp"

namespace SceneUtil
{
    namespace
    {
        const std::string sBinName = "LampShadowsBin";

        // The faces, looking along +X, -X, +Y, -Y, +Z, -Z with these ups (as HeldLightShadow's). The shaders find a
        // face's texture coordinates from its side and up axes (lampShadowFaceS / lampShadowFaceU).
        const std::array<osg::Vec3f, 6> sFaceDirections = { osg::Vec3f(1, 0, 0), osg::Vec3f(-1, 0, 0),
            osg::Vec3f(0, 1, 0), osg::Vec3f(0, -1, 0), osg::Vec3f(0, 0, 1), osg::Vec3f(0, 0, -1) };
        const std::array<osg::Vec3f, 6> sFaceUps = { osg::Vec3f(0, -1, 0), osg::Vec3f(0, -1, 0), osg::Vec3f(0, 0, 1),
            osg::Vec3f(0, 0, -1), osg::Vec3f(0, -1, 0), osg::Vec3f(0, -1, 0) };

        // Lamps whose still faces may be drawn in one frame: spreads a room's worth over a few frames as it loads.
        constexpr int sLampsDrawnPerFrame = 2;

        // A lamp that has moved this many times is left without shadows (it is carried along by something).
        constexpr int sMaxMoves = 3;

        // Frames a slot rests after it is let go, before it takes another lamp: the lamp it had may still be pointed
        // at it in the light data the draw thread is using.
        constexpr std::size_t sRestFrames = 3;

        class FaceCullCallback : public osg::NodeCallback
        {
        public:
            FaceCullCallback(osg::StateSet* binStateSet, LightManager* lightManager)
                : mBinStateSet(binStateSet)
                , mLightManager(lightManager)
            {
            }

            void operator()(osg::Node* /*node*/, osg::NodeVisitor* nv) override
            {
                osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(nv);
                cv->pushStateSet(mBinStateSet);
                mLightManager->osg::Group::traverse(*cv);
                cv->popStateSet();
            }

        private:
            osg::ref_ptr<osg::StateSet> mBinStateSet;
            LightManager* mLightManager;
        };

        class LampShadowsCullCallback : public osg::NodeCallback
        {
        public:
            explicit LampShadowsCullCallback(LampShadows* shadows)
                : mShadows(shadows)
            {
            }

            void operator()(osg::Node* node, osg::NodeVisitor* nv) override
            {
                mShadows->cull(static_cast<LightManager*>(node), static_cast<osgUtil::CullVisitor*>(nv));
                traverse(node, nv);
            }

        private:
            LampShadows* mShadows;
        };

        // Where the characters are (the live faces' casters): the bounds of the nodes with only casters' masks under
        // the lit scene, in world space (the cells' roots carry no transform). Doesn't look inside them.
        class CasterBoundsVisitor : public osg::NodeVisitor
        {
        public:
            explicit CasterBoundsVisitor(unsigned int mask)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mMask(mask)
            {
                setTraversalMask(mask);
            }

            void apply(osg::Node& node) override
            {
                if ((node.getNodeMask() | mMask) == mMask)
                {
                    const osg::BoundingSphere& bound = node.getBound();
                    if (bound.valid())
                        mBounds.push_back(bound);
                    return;
                }
                traverse(node);
            }

            std::vector<osg::BoundingSphere> mBounds;

        private:
            unsigned int mMask;
        };

        osg::ref_ptr<osg::Texture2DArray> makeFaceTexture(int resolution, std::size_t lamps)
        {
            osg::ref_ptr<osg::Texture2DArray> texture = new osg::Texture2DArray;
            texture->setTextureSize(resolution, resolution, static_cast<int>(lamps * 6));
            texture->setInternalFormat(GL_DEPTH_COMPONENT24);
            texture->setSourceFormat(GL_DEPTH_COMPONENT);
            texture->setSourceType(GL_UNSIGNED_INT);
            texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
            texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
            texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            texture->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);
            texture->setShadowComparison(true);
            texture->setShadowCompareFunc(osg::Texture::LEQUAL);
            return texture;
        }
    }

    LampShadows::LampShadows(Shader::ShaderManager& shaderManager, int textureUnit, unsigned int stillMask,
        unsigned int liveMask, int resolution)
        : mStill(sMaxLamps)
        , mLive(sLiveLamps)
        , mStillMask(stillMask)
        , mLiveMask(liveMask)
        , mResolution(resolution)
        , mTextureUnit(textureUnit)
        // a texel and a half of margin round each face, so filtering near its edge stays on it
        , mTanHalfFov(1.f / (1.f - 3.f / static_cast<float>(resolution)))
    {
        mStillTexture = makeFaceTexture(resolution, sMaxLamps);
        mLiveTexture = makeFaceTexture(resolution, sLiveLamps);

        const auto makeCameras = [&](std::vector<Slot>& slots, osg::Texture2DArray* texture) {
            for (std::size_t slot = 0; slot < slots.size(); ++slot)
                for (std::size_t face = 0; face < 6; ++face)
                {
                    osg::ref_ptr<osg::Camera> camera = new osg::Camera;
                    camera->setName("LampShadowCamera");
                    camera->setReferenceFrame(osg::Camera::ABSOLUTE_RF);
                    camera->setRenderOrder(osg::Camera::PRE_RENDER);
                    camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
                    camera->setImplicitBufferAttachmentMask(0, 0);
                    camera->setDrawBuffer(GL_NONE);
                    camera->setReadBuffer(GL_NONE);
                    camera->setClearMask(GL_DEPTH_BUFFER_BIT);
                    camera->setClearDepth(1.0);
                    camera->setViewport(0, 0, resolution, resolution);
                    camera->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
                    camera->setCullingMode(camera->getCullingMode() & ~osg::CullSettings::SMALL_FEATURE_CULLING);
                    camera->attach(
                        osg::Camera::DEPTH_BUFFER, texture, 0, static_cast<unsigned int>(slot * 6 + face));
                    slots[slot].mCameras[face] = camera;
                }
        };
        makeCameras(mStill, mStillTexture);
        makeCameras(mLive, mLiveTexture);

        // Drawn as HeldLightShadow's faces are (the sun's shadow casting shaders, depth only, pushed back a little
        // against shadow acne), but only the sides of faces turned towards the lamp: a lamp inside its own fixture (a
        // lantern static, a sconce's cage) or set a little into the wall behind it sees those from inside, and they
        // don't hide it.
        ShadowsBin::CastingPrograms castingPrograms;
        osg::ref_ptr<osg::Shader> castingVertexShader = shaderManager.getShader("shadowcasting.vert");
        const std::string useGPUShader4 = getGLExtensions().isGpuShader4Supported ? "1" : "0";
        for (int alphaFunc = GL_NEVER; alphaFunc <= GL_ALWAYS; ++alphaFunc)
            castingPrograms[alphaFunc - GL_NEVER] = shaderManager.getProgram(castingVertexShader,
                shaderManager.getShader("shadowcasting.frag",
                    { { "alphaFunc", std::to_string(alphaFunc) }, { "alphaToCoverage", "0" }, { "adjustCoverage", "1" },
                        { "useGPUShader4", useGPUShader4 } }));

        if (osgUtil::RenderBin::getRenderBinPrototype(sBinName) == nullptr)
            osgUtil::RenderBin::addRenderBinPrototype(sBinName, new ShadowsBin(castingPrograms));
        mBinStateSet = new osg::StateSet;
        mBinStateSet->setRenderBinDetails(
            osg::StateSet::OPAQUE_BIN, sBinName, osg::StateSet::OVERRIDE_PROTECTED_RENDERBIN_DETAILS);

        osg::ref_ptr<osg::Image> white = new osg::Image;
        white->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        white->data()[0] = white->data()[1] = white->data()[2] = white->data()[3] = 255;
        osg::ref_ptr<osg::Texture2D> fallback = new osg::Texture2D(white);

        mCastingStateSet = new osg::StateSet;
        mCastingStateSet->setAttributeAndModes(
            castingPrograms[GL_ALWAYS - GL_NEVER], osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        mCastingStateSet->setTextureAttribute(0, fallback, osg::StateAttribute::ON);
        mCastingStateSet->addUniform(new osg::Uniform("useDiffuseMapForShadowAlpha", true));
        mCastingStateSet->addUniform(new osg::Uniform("alphaTestShadows", false));
        osg::ref_ptr<osg::Depth> depth = new osg::Depth(osg::Depth::LESS);
        depth->setWriteMask(true);
        mCastingStateSet->setAttribute(depth, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        mCastingStateSet->setAttribute(
            new osg::ClipControl(osg::ClipControl::LOWER_LEFT, osg::ClipControl::NEGATIVE_ONE_TO_ONE),
            osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        mCastingStateSet->setAttributeAndModes(
            new osg::CullFace(osg::CullFace::BACK), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        mCastingStateSet->setAttributeAndModes(
            new osg::PolygonOffset(1.5f, 4.f), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
    }

    LampShadows::~LampShadows()
    {
        osg::ref_ptr<LightManager> lightManager;
        if (mInstalledOn.lock(lightManager) && mCullCallback != nullptr)
            lightManager->removeCullCallback(mCullCallback);
        releaseAll(0);
    }

    void LampShadows::install(LightManager* lightManager, osg::StateSet* receivers)
    {
        for (std::vector<Slot>* slots : { &mStill, &mLive })
            for (Slot& slot : *slots)
                for (osg::ref_ptr<osg::Camera>& camera : slot.mCameras)
                    camera->setCullCallback(new FaceCullCallback(mBinStateSet, lightManager));

        receivers->setTextureAttribute(mTextureUnit, mStillTexture, osg::StateAttribute::ON);
        receivers->setTextureAttribute(mTextureUnit + 1, mLiveTexture, osg::StateAttribute::ON);
        receivers->addUniform(new osg::Uniform("lampShadowStill", mTextureUnit));
        receivers->addUniform(new osg::Uniform("lampShadowLive", mTextureUnit + 1));
        receivers->addUniform(new osg::Uniform(
            "lampShadowParams", osg::Vec3f(sNearPlane, static_cast<float>(mResolution), mTanHalfFov)));
        osg::ref_ptr<osg::Uniform> sides = new osg::Uniform(osg::Uniform::FLOAT_VEC3, "lampShadowFaceS", 6);
        osg::ref_ptr<osg::Uniform> ups = new osg::Uniform(osg::Uniform::FLOAT_VEC3, "lampShadowFaceU", 6);
        for (unsigned int face = 0; face < 6; ++face)
        {
            // the face camera's own axes (osg::Matrix::lookAt): x along direction x up, y along x x direction
            osg::Vec3f side = sFaceDirections[face] ^ sFaceUps[face];
            side.normalize();
            const osg::Vec3f up = side ^ sFaceDirections[face];
            sides->setElement(face, side);
            ups->setElement(face, up);
        }
        receivers->addUniform(sides);
        receivers->addUniform(ups);

        mCullCallback = new LampShadowsCullCallback(this);
        lightManager->addCullCallback(mCullCallback);
        mInstalledOn = lightManager;
    }

    void LampShadows::setFocus(const osg::Vec3f& position, bool enabled)
    {
        mFocus = position;
        mEnabled = enabled;
    }

    void LampShadows::cull(LightManager* lightManager, osgUtil::CullVisitor* cv)
    {
        // Only the main view draws the faces, once a frame.
        const osg::Camera* camera = cv->getCurrentCamera();
        if (mInFaces || camera == nullptr || camera->getName() != Constants::SceneCamera)
            return;
        const std::size_t frame = cv->getTraversalNumber();
        if (frame == mLastFrame)
            return;
        mLastFrame = frame;
        cullLamps(lightManager, cv);
    }

    void LampShadows::release(Slot& slot, std::size_t frame)
    {
        osg::ref_ptr<LightSource> light;
        if (slot.mLight.lock(light))
            light->setLampShadow(-1, -1, 0.f);
        slot.mLight = nullptr;
        slot.mDrawn = false;
        slot.mPending = false;
        slot.mMoves = 0;
        slot.mFreedFrame = frame;
    }

    void LampShadows::releaseAll(std::size_t frame)
    {
        for (Slot& slot : mStill)
            if (slot.mLight != nullptr)
                release(slot, frame);
        for (Slot& slot : mLive)
            if (slot.mLight != nullptr)
            {
                slot.mLight = nullptr;
                slot.mDrawn = false;
                slot.mFreedFrame = frame;
            }
    }

    unsigned int LampShadows::facesSeeing(const Slot& slot, const std::vector<osg::BoundingSphere>& casters) const
    {
        const osg::Matrixd projection = osg::Matrixd::perspective(
            2.0 * osg::RadiansToDegrees(std::atan(static_cast<double>(mTanHalfFov))), 1.0, sNearPlane, slot.mFar);
        unsigned int faces = 0;
        for (osg::BoundingSphere caster : casters)
        {
            // an animated character's bound can lag its pose (a raised arm, a step): a margin, so a face isn't left
            // out while part of them is in it
            caster.radius() = std::max(caster.radius(), 48.f) + 24.f;
            const float reach = slot.mFar + caster.radius();
            if ((caster.center() - slot.mPosition).length2() > reach * reach)
                continue;
            for (unsigned int face = 0; face < 6; ++face)
            {
                if (faces & (1u << face))
                    continue;
                osg::Polytope polytope;
                polytope.setToUnitFrustum();
                polytope.transformProvidingInverse(
                    osg::Matrixd::lookAt(slot.mPosition, slot.mPosition + sFaceDirections[face], sFaceUps[face])
                    * projection);
                if (polytope.contains(caster))
                    faces |= 1u << face;
            }
        }
        return faces;
    }

    void LampShadows::drawFaces(Slot& slot, osgUtil::CullVisitor* cv, unsigned int mask, unsigned int faces)
    {
        const osg::Matrixd projection = osg::Matrixd::perspective(
            2.0 * osg::RadiansToDegrees(std::atan(static_cast<double>(mTanHalfFov))), 1.0, sNearPlane, slot.mFar);
        const unsigned int traversalMask = cv->getTraversalMask();
        cv->setTraversalMask(mask);
        cv->pushStateSet(mCastingStateSet);
        mInFaces = true;
        for (std::size_t face = 0; face < 6; ++face)
        {
            if (!(faces & (1u << face)))
                continue;
            osg::Camera& faceCamera = *slot.mCameras[face];
            faceCamera.setViewMatrixAsLookAt(
                slot.mPosition, slot.mPosition + sFaceDirections[face], sFaceUps[face]);
            faceCamera.setProjectionMatrix(projection);
            faceCamera.accept(*cv);
        }
        mInFaces = false;
        cv->popStateSet();
        cv->setTraversalMask(traversalMask);
    }

    void LampShadows::cullLamps(LightManager* lightManager, osgUtil::CullVisitor* cv)
    {
        const std::size_t frame = cv->getTraversalNumber();
        if (!mEnabled)
        {
            releaseAll(frame);
            mMoving.clear();
            return;
        }
        std::erase_if(mMoving, [](const osg::observer_ptr<LightSource>& light) { return light == nullptr; });

        // The lamps: lights placed in the world (carried by no one, without a held light's own bounce, which the
        // player's worn lantern light has too), not negative ones, not the player's (HeldLightShadow's), not ones
        // found to move. The nearest to the player first: those whose light reaches the player, then by how far
        // its light ends from them.
        struct Candidate
        {
            LightSource* mLight;
            osg::Vec3f mPosition;
            float mFar;
            float mScore;
        };
        std::vector<Candidate> candidates;
        const float radiusMultiplier = lightManager->getPointLightRadiusMultiplier();
        for (const LightManager::LightSourceTransform& transform : lightManager->getLightsThisFrame())
        {
            LightSource* light = transform.mLightSource;
            if (light->getCastsShadows() || light->getCarrier() != nullptr || light->getBounce() >= 0.f
                || light->getLight(0)->getDiffuse().x() < 0.f)
                continue;
            if (std::any_of(mMoving.begin(), mMoving.end(),
                    [&](const osg::observer_ptr<LightSource>& moving) { return moving == light; }))
                continue;
            const osg::Vec3f position = transform.mWorldMatrix.getTrans();
            const float far = std::max(sNearPlane * 2.f, light->getRadius() * radiusMultiplier);
            // not clamped at 0: lamps whose light reaches the player would all tie, and swap places from frame to frame
            float score = (position - mFocus).length() - far;
            // the lamps that have faces keep them over ones about as near (no drawing back and forth at the edge)
            if (std::any_of(mStill.begin(), mStill.end(), [&](const Slot& slot) { return slot.mLight == light; }))
                score -= 512.f;
            candidates.push_back({ light, position, far, score });
        }
        std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) { return left.mScore < right.mScore; });
        if (candidates.size() > sMaxLamps)
            candidates.resize(sMaxLamps);

        // Keep the lamps that have slots; let go of the ones no longer among the nearest (or gone).
        std::vector<bool> placed(candidates.size(), false);
        for (Slot& slot : mStill)
        {
            osg::ref_ptr<LightSource> light;
            if (!slot.mLight.lock(light))
            {
                if (slot.mDrawn || slot.mPending)
                    release(slot, frame);
                continue;
            }
            auto found = std::find_if(candidates.begin(), candidates.end(),
                [&](const Candidate& candidate) { return candidate.mLight == light.get(); });
            if (found == candidates.end())
            {
                release(slot, frame);
                continue;
            }
            placed[static_cast<std::size_t>(found - candidates.begin())] = true;
            slot.mFar = found->mFar;
            if ((found->mPosition - slot.mPosition).length2() > 1.f)
            {
                if (++slot.mMoves > sMaxMoves)
                {
                    mMoving.emplace_back(light.get());
                    release(slot, frame);
                    continue;
                }
                slot.mPosition = found->mPosition;
                slot.mPending = true;
            }
        }
        // New lamps take free slots that have rested.
        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            if (placed[i])
                continue;
            auto free = std::find_if(mStill.begin(), mStill.end(), [&](const Slot& slot) {
                return slot.mLight == nullptr && frame - slot.mFreedFrame >= sRestFrames;
            });
            if (free == mStill.end())
                break;
            free->mLight = candidates[i].mLight;
            free->mPosition = candidates[i].mPosition;
            free->mFar = candidates[i].mFar;
            free->mPending = true;
            free->mDrawn = false;
            free->mMoves = 0;
        }

        // Draw the still faces of a few waiting lamps, nearest first.
        int drawn = 0;
        for (const Candidate& candidate : candidates)
        {
            if (drawn >= sLampsDrawnPerFrame)
                break;
            for (Slot& slot : mStill)
                if (slot.mPending && slot.mLight == candidate.mLight)
                {
                    drawFaces(slot, cv, mStillMask);
                    slot.mPending = false;
                    if (!slot.mDrawn)
                        slot.mDrawnFrame = frame;
                    slot.mDrawn = true;
                    ++drawn;
                    break;
                }
        }

        // Live faces: the lamps nearest the player that have still faces also draw the characters around them, every
        // frame. Those that do keep it over ones about as near: a lamp handing its live faces over loses its characters'
        // shadows for a few frames (the slot rests), which would flicker.
        std::vector<std::pair<float, LightSource*>> liveCandidates;
        for (const Candidate& candidate : candidates)
        {
            if (!std::any_of(mStill.begin(), mStill.end(),
                    [&](const Slot& slot) { return slot.mDrawn && slot.mLight == candidate.mLight; }))
                continue;
            float distance = (candidate.mPosition - mFocus).length();
            if (std::any_of(mLive.begin(), mLive.end(),
                    [&](const Slot& slot) { return slot.mLight == candidate.mLight; }))
                distance -= 128.f;
            liveCandidates.emplace_back(distance, candidate.mLight);
        }
        std::sort(liveCandidates.begin(), liveCandidates.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; });
        std::vector<LightSource*> live;
        for (const auto& [distance, light] : liveCandidates)
        {
            if (live.size() >= sLiveLamps)
                break;
            live.push_back(light);
        }
        for (Slot& slot : mLive)
            if (slot.mLight != nullptr && std::find(live.begin(), live.end(), slot.mLight.get()) == live.end())
            {
                slot.mLight = nullptr;
                slot.mDrawn = false;
                slot.mFreedFrame = frame;
            }
        for (LightSource* light : live)
        {
            if (std::any_of(mLive.begin(), mLive.end(), [&](const Slot& slot) { return slot.mLight == light; }))
                continue;
            auto free = std::find_if(mLive.begin(), mLive.end(), [&](const Slot& slot) {
                return slot.mLight == nullptr && frame - slot.mFreedFrame >= sRestFrames;
            });
            if (free == mLive.end())
                break;
            free->mLight = light;
            free->mDrawn = false;
        }
        // Only the faces that see a character now, or saw one when last drawn (drawing clears them); all six when the
        // slot is new (whatever another lamp left in them).
        std::vector<osg::BoundingSphere> casters;
        if (std::any_of(mLive.begin(), mLive.end(), [](const Slot& slot) { return slot.mLight != nullptr; }))
        {
            CasterBoundsVisitor visitor(mLiveMask);
            for (unsigned int i = 0; i < lightManager->getNumChildren(); ++i)
                lightManager->getChild(i)->accept(visitor);
            casters = std::move(visitor.mBounds);
        }
        for (Slot& slot : mLive)
        {
            osg::ref_ptr<LightSource> light;
            if (!slot.mLight.lock(light))
                continue;
            auto still = std::find_if(
                mStill.begin(), mStill.end(), [&](const Slot& stillSlot) { return stillSlot.mLight == light.get(); });
            if (still == mStill.end())
                continue;
            slot.mPosition = still->mPosition;
            slot.mFar = still->mFar;
            const unsigned int seeing = facesSeeing(slot, casters);
            const unsigned int faces = slot.mDrawn ? (seeing | slot.mFacesWithCasters) : 0x3fu;
            if (faces != 0)
                drawFaces(slot, cv, mLiveMask, faces);
            slot.mFacesWithCasters = seeing;
            if (!slot.mDrawn)
                slot.mDrawnFrame = frame;
            slot.mDrawn = true;
        }

        // Point the lamps at their faces, once the faces have been drawn a frame (the light data of this frame may
        // already be filled).
        std::size_t count = 0;
        for (std::size_t i = 0; i < mStill.size(); ++i)
        {
            Slot& slot = mStill[i];
            osg::ref_ptr<LightSource> light;
            if (!slot.mLight.lock(light))
                continue;
            if (!slot.mDrawn || frame <= slot.mDrawnFrame)
            {
                light->setLampShadow(-1, -1, 0.f);
                continue;
            }
            int liveLayer = -1;
            for (std::size_t j = 0; j < mLive.size(); ++j)
                if (mLive[j].mLight == light.get() && mLive[j].mDrawn && frame > mLive[j].mDrawnFrame)
                    liveLayer = static_cast<int>(j * 6);
            light->setLampShadow(static_cast<int>(i * 6), liveLayer, slot.mFar);
            ++count;
        }

        if (count != mLoggedCount && (count == 0 || mLoggedCount == 0 || mLoggedCount == ~std::size_t(0)))
            Log(Debug::Info) << "Lamp shadows: " << count << " lamps cast shadows";
        mLoggedCount = count;
    }
}
