#include "heldlightshadow.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>

#include <osg/ClipControl>
#include <osg/CullFace>
#include <osg/Depth>
#include <osg/GLDefines>
#include <osg/Geode>
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
#include "positionattitudetransform.hpp"
#include "shadowsbin.hpp"

namespace SceneUtil
{
    namespace
    {
        // The lights carried by the player are carried lights (ones with their own light bounce, see addLight) within
        // this distance of them.
        constexpr float sMaxCarrierDistance = 160.f;
        // Faces start this close to the light.
        constexpr float sNearPlane = 4.f;

        const std::string sBinName = "HeldLightShadowsBin";

        // The copy's node mask: no scene node has it, so a face that copies is drawn even with nothing else to.
        constexpr unsigned int sCopyMask = 1u << 31;

        // The faces of a cube map as OpenGL lays them out: looking along +X, -X, +Y, -Y, +Z, -Z with these ups.
        const std::array<osg::Vec3f, 6> sFaceDirections = { osg::Vec3f(1, 0, 0), osg::Vec3f(-1, 0, 0),
            osg::Vec3f(0, 1, 0), osg::Vec3f(0, -1, 0), osg::Vec3f(0, 0, 1), osg::Vec3f(0, 0, -1) };
        const std::array<osg::Vec3f, 6> sFaceUps = { osg::Vec3f(0, -1, 0), osg::Vec3f(0, -1, 0), osg::Vec3f(0, 0, 1),
            osg::Vec3f(0, 0, -1), osg::Vec3f(0, -1, 0), osg::Vec3f(0, -1, 0) };

        // Set when a copy couldn't be made (no glCopyImageSubData): the cache is left off from then on.
        std::atomic<bool> sCopyFailed{ false };

        // Copies one face of the cache's cube map (what doesn't move) into the one the shaders read, before the
        // characters are drawn over it. First in the face's drawing (a render bin of its own, before the shadows bin).
        class CopyFace : public osg::Drawable
        {
        public:
            CopyFace(osg::TextureCubeMap* from, osg::TextureCubeMap* to, unsigned int face, int resolution)
                : mFrom(from)
                , mTo(to)
                , mFace(static_cast<GLint>(face))
                , mResolution(resolution)
            {
                setUseDisplayList(false);
                setSupportsDisplayList(false);
                setCullingActive(false);
            }

            osg::BoundingBox computeBoundingBox() const override
            {
                return osg::BoundingBox(-1.f, -1.f, -1.f, 1.f, 1.f, 1.f);
            }

            void drawImplementation(osg::RenderInfo& renderInfo) const override
            {
                using CopyImageSubData = void(GL_APIENTRY*)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum,
                    GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei);
                static CopyImageSubData copy = nullptr;
                static bool looked = false;
                if (!looked)
                {
                    looked = true;
                    osg::setGLExtensionFuncPtr(copy, "glCopyImageSubData", "glCopyImageSubDataARB");
                    if (copy == nullptr)
                    {
                        sCopyFailed = true;
                        Log(Debug::Warning) << "Lantern shadows: no glCopyImageSubData, the cache is off";
                    }
                }
                const unsigned int contextID = renderInfo.getContextID();
                osg::Texture::TextureObject* from = mFrom->getTextureObject(contextID);
                osg::Texture::TextureObject* to = mTo->getTextureObject(contextID);
                if (copy == nullptr || from == nullptr || to == nullptr)
                    return;
                copy(from->id(), GL_TEXTURE_CUBE_MAP, 0, 0, 0, mFace, to->id(), GL_TEXTURE_CUBE_MAP, 0, 0, 0, mFace,
                    mResolution, mResolution, 1);
            }

        private:
            osg::ref_ptr<osg::TextureCubeMap> mFrom;
            osg::ref_ptr<osg::TextureCubeMap> mTo;
            GLint mFace;
            GLsizei mResolution;
        };

        // Draws the scene into one face: the lit scene's contents, with the shadows bin (see VDSMCameraCullCallback).
        // With the cache, first the copy of what doesn't move.
        class FaceCullCallback : public osg::NodeCallback
        {
        public:
            FaceCullCallback(osg::StateSet* binStateSet, LightManager* lightManager,
                osg::StateSet* copyStateSet = nullptr, osg::Node* copy = nullptr)
                : mBinStateSet(binStateSet)
                , mCopyStateSet(copyStateSet)
                , mCopy(copy)
                , mLightManager(lightManager)
            {
            }

            /// Whether this frame's drawing starts with the copy (set before the face is culled).
            void setCopy(bool copy) { mCopyNow = copy && mCopy != nullptr; }

            void operator()(osg::Node* /*node*/, osg::NodeVisitor* nv) override
            {
                osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(nv);
                if (mCopyNow)
                {
                    cv->pushStateSet(mCopyStateSet);
                    mCopy->accept(*cv);
                    cv->popStateSet();
                }
                cv->pushStateSet(mBinStateSet);
                mLightManager->osg::Group::traverse(*cv);
                cv->popStateSet();
            }

        private:
            osg::ref_ptr<osg::StateSet> mBinStateSet;
            osg::ref_ptr<osg::StateSet> mCopyStateSet;
            osg::ref_ptr<osg::Node> mCopy;
            LightManager* mLightManager;
            bool mCopyNow = false;
        };

        bool copySupported()
        {
            return !sCopyFailed && glExtensionsReady() && getGLExtensions().glVersion >= 4.3f;
        }

        class ShadowCullCallback : public osg::NodeCallback
        {
        public:
            explicit ShadowCullCallback(HeldLightShadow* shadow)
                : mShadow(shadow)
            {
            }

            void operator()(osg::Node* node, osg::NodeVisitor* nv) override
            {
                mShadow->cull(static_cast<LightManager*>(node), static_cast<osgUtil::CullVisitor*>(nv));
                traverse(node, nv);
                static_cast<osgUtil::CullVisitor*>(nv)->popStateSet();
            }

        private:
            HeldLightShadow* mShadow;
        };
    }

    HeldLightShadow::HeldLightShadow(Shader::ShaderManager& shaderManager, int textureUnit, unsigned int castMask,
        int resolution, float minCasterPixels, float cacheDistance, unsigned int liveMask, float maxDistance)
        : mCastMask(castMask)
        , mCurrentCastMask(castMask)
        , mResolution(resolution)
        , mCacheDistance(liveMask != 0 ? cacheDistance : 0.f)
        , mLiveMask(liveMask)
        , mMaxDistance(maxDistance)
        , mTextureUnit(textureUnit)
    {
        const auto makeTexture = [&] {
            osg::ref_ptr<osg::TextureCubeMap> texture = new osg::TextureCubeMap;
            texture->setTextureSize(resolution, resolution);
            texture->setInternalFormat(GL_DEPTH_COMPONENT24);
            texture->setSourceFormat(GL_DEPTH_COMPONENT);
            texture->setSourceType(GL_UNSIGNED_INT);
            texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
            texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
            texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            texture->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);
            return texture;
        };
        for (Slot& slot : mSlots)
        {
            slot.mTexture = makeTexture();
            slot.mTexture->setShadowComparison(true);
            slot.mTexture->setShadowCompareFunc(osg::Texture::LEQUAL);
            if (mCacheDistance > 0.f)
                slot.mStillTexture = makeTexture();

            for (std::size_t i = 0; i < slot.mCameras.size() * 2; ++i)
            {
                const std::size_t face = i % 6;
                const bool still = i >= 6;
                if (still && slot.mStillTexture == nullptr)
                    break;
                osg::ref_ptr<osg::Camera> camera = new osg::Camera;
                camera->setName("HeldLightShadowCamera");
                camera->setReferenceFrame(osg::Camera::ABSOLUTE_RF);
                // the cache's faces before the faces that copy them
                camera->setRenderOrder(osg::Camera::PRE_RENDER, still ? -1 : 0);
                camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
                camera->setImplicitBufferAttachmentMask(0, 0);
                camera->setDrawBuffer(GL_NONE);
                camera->setReadBuffer(GL_NONE);
                camera->setClearMask(GL_DEPTH_BUFFER_BIT);
                camera->setClearDepth(1.0);
                camera->setViewport(0, 0, resolution, resolution);
                camera->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
                // Small things far from the light (a cup across the room) cover a few pixels of a face: left out
                // when asked, they are most of what a cluttered room has to draw six times a frame.
                if (minCasterPixels > 0.f)
                {
                    camera->setCullingMode(camera->getCullingMode() | osg::CullSettings::SMALL_FEATURE_CULLING);
                    camera->setSmallFeatureCullingPixelSize(minCasterPixels);
                }
                else
                    camera->setCullingMode(camera->getCullingMode() & ~osg::CullSettings::SMALL_FEATURE_CULLING);
                camera->attach(osg::Camera::DEPTH_BUFFER, (still ? slot.mStillTexture : slot.mTexture).get(), 0,
                    static_cast<unsigned int>(face));
                (still ? slot.mStillCameras : slot.mCameras)[face] = camera;
            }
        }

        // What the faces are drawn with: the sun's shadow casting shaders (alpha tested where the material is), depth
        // only, the usual (not reversed) depth range, both sides of every face (single sided walls must cast too),
        // pushed back a little against shadow acne.
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
        mCopyStateSet = new osg::StateSet;
        mCopyStateSet->setRenderBinDetails(-1, "RenderBin", osg::StateSet::PROTECTED_RENDERBIN_DETAILS);

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
        mCastingStateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        mCastingStateSet->setAttributeAndModes(
            new osg::PolygonOffset(1.5f, 4.f), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);

        // heldLightShadowPos[i]: where light i is (world space), and in w the far end of its faces (0: no light);
        // heldLightShadowCenter[i]: where its faces were drawn from (where it was, with the cache)
        for (std::size_t i = 0; i < mReceiverStateSets.size(); ++i)
        {
            mReceiverStateSets[i] = new osg::StateSet;
            mPositionUniforms[i]
                = new osg::Uniform(osg::Uniform::FLOAT_VEC4, "heldLightShadowPos", static_cast<int>(sMaxLights));
            mReceiverStateSets[i]->addUniform(mPositionUniforms[i]);
            mCenterUniforms[i]
                = new osg::Uniform(osg::Uniform::FLOAT_VEC4, "heldLightShadowCenter", static_cast<int>(sMaxLights));
            mReceiverStateSets[i]->addUniform(mCenterUniforms[i]);
        }
        mInactiveStateSet = new osg::StateSet;
        osg::ref_ptr<osg::Uniform> inactive
            = new osg::Uniform(osg::Uniform::FLOAT_VEC4, "heldLightShadowPos", static_cast<int>(sMaxLights));
        for (std::size_t i = 0; i < sMaxLights; ++i)
            inactive->setElement(static_cast<unsigned int>(i), osg::Vec4f());
        mInactiveStateSet->addUniform(inactive);
        osg::ref_ptr<osg::Uniform> inactiveCenter
            = new osg::Uniform(osg::Uniform::FLOAT_VEC4, "heldLightShadowCenter", static_cast<int>(sMaxLights));
        for (std::size_t i = 0; i < sMaxLights; ++i)
            inactiveCenter->setElement(static_cast<unsigned int>(i), osg::Vec4f());
        mInactiveStateSet->addUniform(inactiveCenter);
    }

    HeldLightShadow::~HeldLightShadow()
    {
        osg::ref_ptr<LightManager> lightManager;
        if (mInstalledOn.lock(lightManager) && mCullCallback != nullptr)
            lightManager->removeCullCallback(mCullCallback);
        for (Slot& slot : mSlots)
        {
            osg::ref_ptr<LightSource> caster;
            if (slot.mCaster.lock(caster))
                caster->setCastsShadows(false);
        }
    }

    void HeldLightShadow::install(LightManager* lightManager, osg::StateSet* receivers)
    {
        mLightManager = lightManager;
        for (std::size_t i = 0; i < sMaxLights; ++i)
        {
            Slot& slot = mSlots[i];
            for (std::size_t face = 0; face < slot.mCameras.size(); ++face)
            {
                osg::ref_ptr<osg::Geode> copy;
                if (slot.mStillTexture != nullptr)
                {
                    copy = new osg::Geode;
                    copy->addDrawable(
                        new CopyFace(slot.mStillTexture, slot.mTexture, static_cast<unsigned int>(face), mResolution));
                    copy->setCullingActive(false);
                    copy->setNodeMask(sCopyMask);
                }
                slot.mCameras[face]->setCullCallback(
                    new FaceCullCallback(mBinStateSet, lightManager, mCopyStateSet, copy));
                if (slot.mStillCameras[face] != nullptr)
                    slot.mStillCameras[face]->setCullCallback(new FaceCullCallback(mBinStateSet, lightManager));
            }
            const int unit = mTextureUnit + static_cast<int>(i);
            receivers->setTextureAttribute(unit, mSlots[i].mTexture, osg::StateAttribute::ON);
            receivers->addUniform(new osg::Uniform(("heldLightShadowMap" + std::to_string(i)).c_str(), unit));
        }
        receivers->addUniform(mInactiveStateSet->getUniform("heldLightShadowPos"));
        receivers->addUniform(mInactiveStateSet->getUniform("heldLightShadowCenter"));
        // filtered across the edges of the faces, not clamped at them (no seams where one face meets the next)
        receivers->setMode(GL_TEXTURE_CUBE_MAP_SEAMLESS, osg::StateAttribute::ON);
        receivers->addUniform(
            new osg::Uniform("heldLightShadowRange", osg::Vec2f(sNearPlane, static_cast<float>(mResolution))));
        mCullCallback = new ShadowCullCallback(this);
        lightManager->addCullCallback(mCullCallback);
        mInstalledOn = lightManager;
    }

    void HeldLightShadow::setFocus(const osg::Vec3f& position, bool enabled, unsigned int castMask)
    {
        mFocus = position;
        mEnabled = enabled;
        mCurrentCastMask = castMask != 0 ? castMask : mCastMask;
    }

    void HeldLightShadow::cull(LightManager* lightManager, osgUtil::CullVisitor* cv)
    {
        // Only the main view draws the faces, once a frame; other views (the water's reflection, the sun's shadows)
        // light without them. ShadowCullCallback pops what is pushed here.
        const osg::Camera* camera = cv->getCurrentCamera();
        const std::size_t frame = cv->getTraversalNumber();
        if (mInFaces || camera == nullptr || camera->getName() != Constants::SceneCamera)
        {
            cv->pushStateSet(mInactiveStateSet);
            return;
        }
        if (frame != mLastFrame)
        {
            mLastFrame = frame;
            cullFaces(lightManager, cv);
        }
        cv->pushStateSet(mActive ? mReceiverStateSets[frame % 2].get() : mInactiveStateSet.get());
    }

    void HeldLightShadow::cullFaces(LightManager* lightManager, osgUtil::CullVisitor* cv)
    {
        mActive = false;

        // The player's own lights: carried by the player, or carried by no one (a worn lantern's stand-in light, which
        // mods move along with the player), not an NPC's torch. The nearest ones.
        std::array<const LightManager::LightSourceTransform*, sMaxLights> casters{};
        std::array<float, sMaxLights> distances;
        distances.fill(sMaxCarrierDistance * sMaxCarrierDistance);
        if (mEnabled)
        {
            for (const LightManager::LightSourceTransform& light : lightManager->getLightsThisFrame())
            {
                // Not the "empty" test (no model of its own): a worn lantern's stand-in light has none, by design.
                // Negative lights take light away and cast no shadow.
                if (light.mLightSource->getBounce() < 0.f || light.mLightSource->getLight(0)->getDiffuse().x() < 0.f)
                    continue;
                const PositionAttitudeTransform* carrier = light.mLightSource->getCarrier();
                if (carrier != nullptr && (carrier->getPosition() - mFocus).length2() > 1.f)
                    continue;
                float distance2 = (light.mWorldMatrix.getTrans() - mFocus).length2();
                const LightManager::LightSourceTransform* candidate = &light;
                for (std::size_t i = 0; i < sMaxLights; ++i)
                    if (distance2 < distances[i])
                    {
                        std::swap(distance2, distances[i]);
                        std::swap(candidate, casters[i]);
                    }
            }
        }

        // What casts shadows, in the log when it changes (a light lit or put out, entering or leaving an interior)
        std::size_t count = 0;
        for (const LightManager::LightSourceTransform* light : casters)
            if (light != nullptr)
                ++count;
        if (count != mLoggedCount)
        {
            mLoggedCount = count;
            Log(Debug::Info) << "Lantern shadows: " << count << " of the player's lights cast shadows"
                             << (mEnabled ? "" : " (off outdoors)");
        }

        const std::size_t frame = cv->getTraversalNumber();
        const std::size_t buffer = frame % 2;
        const unsigned int traversalMask = cv->getTraversalMask();
        // (outside, with nothing that doesn't move to keep, there's nothing to cache)
        const bool cache = mCacheDistance > 0.f && (mCurrentCastMask & ~mLiveMask) != 0 && copySupported();

        // The sides of the view (no near or far planes: those differ with reverse z), in world space: a face of a
        // light's cube map whose whole pyramid is outside one of them shades nothing on screen this frame, and isn't
        // drawn (it keeps what it last had, which nothing on screen looks up).
        osg::Polytope view;
        if (const osg::Camera* camera = cv->getCurrentCamera())
        {
            view.setToUnitFrustum(false, false);
            view.transformProvidingInverse(camera->getViewMatrix() * camera->getProjectionMatrix());
        }
        const auto faceSeen = [&](const osg::Vec3f& position, std::size_t face, float farPlane) {
            if (view.getPlaneList().empty())
                return true;
            const osg::Vec3f& dir = sFaceDirections[face];
            const osg::Vec3f& up = sFaceUps[face];
            const osg::Vec3f right = dir ^ up;
            // a quarter wider than the face: lookups near its edges (the softening taps, seamless filtering) reach
            // into the faces next to it
            constexpr float spread = 1.25f;
            const std::array<osg::Vec3f, 5> corners = {
                position,
                position + (dir + (right + up) * spread) * farPlane,
                position + (dir + (right - up) * spread) * farPlane,
                position + (dir - (right + up) * spread) * farPlane,
                position + (dir - (right - up) * spread) * farPlane,
            };
            for (const osg::Plane& plane : view.getPlaneList())
                if (std::all_of(corners.begin(), corners.end(),
                        [&](const osg::Vec3f& corner) { return plane.distance(corner) < 0.f; }))
                    return false;
            return true;
        };
        for (std::size_t i = 0; i < sMaxLights; ++i)
        {
            Slot& slot = mSlots[i];
            LightSource* caster = casters[i] != nullptr ? casters[i]->mLightSource : nullptr;
            osg::ref_ptr<LightSource> previous;
            if (slot.mCaster.lock(previous) && previous != caster
                && std::none_of(casters.begin(), casters.end(), [&](const LightManager::LightSourceTransform* light) {
                       return light != nullptr && light->mLightSource == previous.get();
                   }))
                previous->setCastsShadows(false);
            slot.mCaster = caster;
            if (caster == nullptr)
            {
                mPositionUniforms[buffer]->setElement(static_cast<unsigned int>(i), osg::Vec4f());
                slot.mAnchorLight = nullptr;
                slot.mStillReady = false;
                continue;
            }
            caster->setCastsShadows(true);

            const osg::Vec3f actual = casters[i]->mWorldMatrix.getTrans();
            const float reach = caster->getRadius() * lightManager->getPointLightRadiusMultiplier();
            // held light shadow distance: the shadows end there, fading out over the last fifth, the light reaching on
            const bool capped = mMaxDistance > 0.f && mMaxDistance < reach;
            const float actualFar = std::max(sNearPlane * 2.f, capped ? mMaxDistance : reach);

            // The cache: while the light stays near where it was (standing, a torch swaying in hand), its faces are
            // drawn from there; what doesn't move is drawn once into the cache's faces, then one of those a frame
            // (a door opened, an item taken), and each frame the faces seen copy them and draw the characters.
            // Further than that, or another light, the faces are drawn whole from where the light is, as without.
            osg::Vec3f position = actual;
            float farPlane = actualFar;
            bool cached = false;
            bool buildStill = false;
            if (cache)
            {
                const bool anchored = slot.mAnchorLight == caster && slot.mAnchorMask == mCurrentCastMask
                    && slot.mAnchorFrame + 1 == frame && std::abs(actualFar - slot.mAnchorFar) <= actualFar * 0.02f
                    && (actual - slot.mAnchor).length2() <= mCacheDistance * mCacheDistance;
                if (anchored)
                {
                    position = slot.mAnchor;
                    farPlane = slot.mAnchorFar;
                    cached = true;
                    buildStill = !slot.mStillReady;
                    slot.mStillReady = true;
                }
                else
                {
                    slot.mAnchorLight = caster;
                    slot.mAnchor = actual;
                    slot.mAnchorFar = actualFar;
                    slot.mAnchorMask = mCurrentCastMask;
                    slot.mStillReady = false;
                }
                slot.mAnchorFrame = frame;
            }
            else
            {
                slot.mAnchorLight = nullptr;
                slot.mStillReady = false;
            }

            const osg::Matrixd projection = osg::Matrixd::perspective(90.0, 1.0, sNearPlane, farPlane);
            const auto drawFace = [&](osg::Camera& faceCamera, std::size_t face, unsigned int mask, bool copy) {
                if (auto* callback = dynamic_cast<FaceCullCallback*>(faceCamera.getCullCallback()))
                    callback->setCopy(copy);
                faceCamera.setClearMask(copy ? 0 : GL_DEPTH_BUFFER_BIT);
                faceCamera.setViewMatrixAsLookAt(position, position + sFaceDirections[face], sFaceUps[face]);
                faceCamera.setProjectionMatrix(projection);
                cv->setTraversalMask(mask);
                faceCamera.accept(*cv);
            };

            cv->pushStateSet(mCastingStateSet);
            mInFaces = true;
            if (cached)
            {
                const unsigned int stillMask = mCurrentCastMask & ~mLiveMask;
                for (std::size_t face = 0; face < slot.mStillCameras.size(); ++face)
                    if (buildStill || face == slot.mNextStillFace)
                        drawFace(*slot.mStillCameras[face], face, stillMask, false);
                if (!buildStill)
                    slot.mNextStillFace = (slot.mNextStillFace + 1) % slot.mStillCameras.size();
            }
            const unsigned int faceMask = cached ? (mCurrentCastMask & mLiveMask) | sCopyMask : mCurrentCastMask;
            for (std::size_t face = 0; face < slot.mCameras.size(); ++face)
            {
                if (!faceSeen(position, face, farPlane))
                    continue;
                drawFace(*slot.mCameras[face], face, faceMask, cached);
            }
            mInFaces = false;
            cv->popStateSet();
            cv->setTraversalMask(traversalMask);

            mPositionUniforms[buffer]->setElement(static_cast<unsigned int>(i), osg::Vec4f(actual, farPlane));
            mCenterUniforms[buffer]->setElement(
                static_cast<unsigned int>(i), osg::Vec4f(position, capped ? farPlane * 0.8f : 0.f));
            mActive = true;
        }
    }
}
