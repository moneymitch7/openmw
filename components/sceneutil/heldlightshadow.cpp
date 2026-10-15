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
        // The light carried by the player is the carried light (one with its own light bounce, see addLight) nearest
        // to the player and within this distance of them.
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
        mTexture = new osg::TextureCubeMap;
        mTexture->setTextureSize(resolution, resolution);
        mTexture->setInternalFormat(GL_DEPTH_COMPONENT24);
        mTexture->setSourceFormat(GL_DEPTH_COMPONENT);
        mTexture->setSourceType(GL_UNSIGNED_INT);
        mTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        mTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        mTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        mTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        mTexture->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);
        mTexture->setShadowComparison(true);
        mTexture->setShadowCompareFunc(osg::Texture::LEQUAL);

        for (std::size_t face = 0; face < mCameras.size(); ++face)
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
            camera->attach(osg::Camera::DEPTH_BUFFER, mTexture.get(), 0, static_cast<unsigned int>(face));
            mCameras[face] = camera;
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

        for (std::size_t i = 0; i < mReceiverStateSets.size(); ++i)
        {
            mReceiverStateSets[i] = new osg::StateSet;
            mPositionUniforms[i] = new osg::Uniform("heldLightShadowPos", osg::Vec4f());
            mRangeUniforms[i] = new osg::Uniform("heldLightShadowRange", osg::Vec3f(sNearPlane, 1.f, resolution));
            mReceiverStateSets[i]->addUniform(mPositionUniforms[i]);
            mReceiverStateSets[i]->addUniform(mRangeUniforms[i]);
        }
        mInactiveStateSet = new osg::StateSet;
        mInactiveStateSet->addUniform(new osg::Uniform("heldLightShadowPos", osg::Vec4f()));
    }

    HeldLightShadow::~HeldLightShadow()
    {
        osg::ref_ptr<LightManager> lightManager;
        if (mInstalledOn.lock(lightManager) && mCullCallback != nullptr)
            lightManager->removeCullCallback(mCullCallback);
        osg::ref_ptr<LightSource> caster;
        if (mCaster.lock(caster))
            caster->setCastsShadows(false);
    }

    void HeldLightShadow::install(LightManager* lightManager, osg::StateSet* receivers)
    {
        mLightManager = lightManager;
        for (osg::ref_ptr<osg::Camera>& camera : mCameras)
            camera->setCullCallback(new FaceCullCallback(mBinStateSet, lightManager));
        receivers->setTextureAttribute(mTextureUnit, mTexture, osg::StateAttribute::ON);
        receivers->addUniform(new osg::Uniform("heldLightShadowMap", mTextureUnit));
        receivers->addUniform(new osg::Uniform("heldLightShadowPos", osg::Vec4f()));
        receivers->addUniform(new osg::Uniform("heldLightShadowRange", osg::Vec3f(sNearPlane, 1.f, mResolution)));
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
        LightSource* caster = nullptr;
        osg::Vec3f position;
        if (mEnabled)
        {
            // the player's own: carried by the player, or carried by no one (a worn lantern's stand-in light, which
            // mods move along with the player), not an NPC's torch
            float best = sMaxCarrierDistance * sMaxCarrierDistance;
            for (const LightManager::LightSourceTransform& light : lightManager->getLightsThisFrame())
            {
                if (light.mLightSource->getBounce() < 0.f || light.mLightSource->getEmpty())
                    continue;
                const PositionAttitudeTransform* carrier = light.mLightSource->getCarrier();
                if (carrier != nullptr && (carrier->getPosition() - mFocus).length2() > 1.f)
                    continue;
                const osg::Vec3f lightPos = light.mWorldMatrix.getTrans();
                const float distance2 = (lightPos - mFocus).length2();
                if (distance2 < best)
                {
                    best = distance2;
                    caster = light.mLightSource;
                    position = lightPos;
                }
            }
        }

        osg::ref_ptr<LightSource> previous;
        if (mCaster.lock(previous) && previous != caster)
            previous->setCastsShadows(false);
        mCaster = caster;
        if (caster == nullptr)
            return;
        caster->setCastsShadows(true);

        const float farPlane
            = std::max(sNearPlane * 2.f, caster->getRadius() * lightManager->getPointLightRadiusMultiplier());
        const osg::Matrixd projection = osg::Matrixd::perspective(90.0, 1.0, sNearPlane, farPlane);

        const unsigned int traversalMask = cv->getTraversalMask();
        cv->setTraversalMask(mCastMask);
        cv->pushStateSet(mCastingStateSet);
        mInFaces = true;
        for (std::size_t face = 0; face < mCameras.size(); ++face)
        {
            osg::Camera& faceCamera = *mCameras[face];
            faceCamera.setViewMatrixAsLookAt(position, position + sFaceDirections[face], sFaceUps[face]);
            faceCamera.setProjectionMatrix(projection);
            faceCamera.accept(*cv);
        }
        mInFaces = false;
        cv->popStateSet();
        cv->setTraversalMask(traversalMask);

        const std::size_t buffer = cv->getTraversalNumber() % 2;
        mPositionUniforms[buffer]->set(osg::Vec4f(position, 1.f));
        mRangeUniforms[buffer]->set(osg::Vec3f(sNearPlane, farPlane, static_cast<float>(mResolution)));
        mActive = true;
    }
}
