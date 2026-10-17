#include "heldlightshadow.hpp"

#include <algorithm>
#include <string>

#include <osg/ClipControl>
#include <osg/CullFace>
#include <osg/Depth>
#include <osg/Image>
#include <osg/PolygonOffset>
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

        // The faces of a cube map as OpenGL lays them out: looking along +X, -X, +Y, -Y, +Z, -Z with these ups.
        const std::array<osg::Vec3f, 6> sFaceDirections = { osg::Vec3f(1, 0, 0), osg::Vec3f(-1, 0, 0),
            osg::Vec3f(0, 1, 0), osg::Vec3f(0, -1, 0), osg::Vec3f(0, 0, 1), osg::Vec3f(0, 0, -1) };
        const std::array<osg::Vec3f, 6> sFaceUps = { osg::Vec3f(0, -1, 0), osg::Vec3f(0, -1, 0), osg::Vec3f(0, 0, 1),
            osg::Vec3f(0, 0, -1), osg::Vec3f(0, -1, 0), osg::Vec3f(0, -1, 0) };

        // Draws the scene into one face: the lit scene's contents, with the shadows bin (see VDSMCameraCullCallback).
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

    HeldLightShadow::HeldLightShadow(
        Shader::ShaderManager& shaderManager, int textureUnit, unsigned int castMask, int resolution)
        : mCastMask(castMask)
        , mResolution(resolution)
        , mTextureUnit(textureUnit)
    {
        for (Slot& slot : mSlots)
        {
            slot.mTexture = new osg::TextureCubeMap;
            osg::TextureCubeMap& texture = *slot.mTexture;
            texture.setTextureSize(resolution, resolution);
            texture.setInternalFormat(GL_DEPTH_COMPONENT24);
            texture.setSourceFormat(GL_DEPTH_COMPONENT);
            texture.setSourceType(GL_UNSIGNED_INT);
            texture.setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
            texture.setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
            texture.setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            texture.setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            texture.setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);
            texture.setShadowComparison(true);
            texture.setShadowCompareFunc(osg::Texture::LEQUAL);

            for (std::size_t face = 0; face < slot.mCameras.size(); ++face)
            {
                osg::ref_ptr<osg::Camera> camera = new osg::Camera;
                camera->setName("HeldLightShadowCamera");
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
                camera->attach(osg::Camera::DEPTH_BUFFER, slot.mTexture.get(), 0, static_cast<unsigned int>(face));
                slot.mCameras[face] = camera;
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

        // heldLightShadowPos[i]: where light i is (world space), and in w the far end of its faces (0: no light)
        for (std::size_t i = 0; i < mReceiverStateSets.size(); ++i)
        {
            mReceiverStateSets[i] = new osg::StateSet;
            mPositionUniforms[i]
                = new osg::Uniform(osg::Uniform::FLOAT_VEC4, "heldLightShadowPos", static_cast<int>(sMaxLights));
            mReceiverStateSets[i]->addUniform(mPositionUniforms[i]);
        }
        mInactiveStateSet = new osg::StateSet;
        osg::ref_ptr<osg::Uniform> inactive
            = new osg::Uniform(osg::Uniform::FLOAT_VEC4, "heldLightShadowPos", static_cast<int>(sMaxLights));
        for (std::size_t i = 0; i < sMaxLights; ++i)
            inactive->setElement(static_cast<unsigned int>(i), osg::Vec4f());
        mInactiveStateSet->addUniform(inactive);
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
            for (osg::ref_ptr<osg::Camera>& camera : mSlots[i].mCameras)
                camera->setCullCallback(new FaceCullCallback(mBinStateSet, lightManager));
            const int unit = mTextureUnit + static_cast<int>(i);
            receivers->setTextureAttribute(unit, mSlots[i].mTexture, osg::StateAttribute::ON);
            receivers->addUniform(new osg::Uniform(("heldLightShadowMap" + std::to_string(i)).c_str(), unit));
        }
        receivers->addUniform(mInactiveStateSet->getUniform("heldLightShadowPos"));
        receivers->addUniform(
            new osg::Uniform("heldLightShadowRange", osg::Vec2f(sNearPlane, static_cast<float>(mResolution))));
        mCullCallback = new ShadowCullCallback(this);
        lightManager->addCullCallback(mCullCallback);
        mInstalledOn = lightManager;
    }

    void HeldLightShadow::setFocus(const osg::Vec3f& position, bool enabled)
    {
        mFocus = position;
        mEnabled = enabled;
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
                if (light.mLightSource->getBounce() < 0.f || light.mLightSource->getEmpty())
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

        const std::size_t buffer = cv->getTraversalNumber() % 2;
        const unsigned int traversalMask = cv->getTraversalMask();
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
                continue;
            }
            caster->setCastsShadows(true);

            const osg::Vec3f position = casters[i]->mWorldMatrix.getTrans();
            const float farPlane
                = std::max(sNearPlane * 2.f, caster->getRadius() * lightManager->getPointLightRadiusMultiplier());
            const osg::Matrixd projection = osg::Matrixd::perspective(90.0, 1.0, sNearPlane, farPlane);

            cv->setTraversalMask(mCastMask);
            cv->pushStateSet(mCastingStateSet);
            mInFaces = true;
            for (std::size_t face = 0; face < slot.mCameras.size(); ++face)
            {
                osg::Camera& faceCamera = *slot.mCameras[face];
                faceCamera.setViewMatrixAsLookAt(position, position + sFaceDirections[face], sFaceUps[face]);
                faceCamera.setProjectionMatrix(projection);
                faceCamera.accept(*cv);
            }
            mInFaces = false;
            cv->popStateSet();
            cv->setTraversalMask(traversalMask);

            mPositionUniforms[buffer]->setElement(static_cast<unsigned int>(i), osg::Vec4f(position, farPlane));
            mActive = true;
        }
    }
}
