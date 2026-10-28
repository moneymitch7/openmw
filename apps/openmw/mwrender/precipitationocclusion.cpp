#include "precipitationocclusion.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

#include <osgUtil/CullVisitor>

#include <components/misc/constants.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/util.hpp>
#include <components/settings/values.hpp>
#include <components/shader/shadermanager.hpp>

#include "../mwbase/environment.hpp"

#include "vismask.hpp"

namespace
{
    class PrecipitationOcclusionUpdater : public SceneUtil::StateSetUpdater
    {
    public:
        PrecipitationOcclusionUpdater(osg::ref_ptr<osg::Texture2D> depthTexture, osg::Camera* depthCamera)
            : mDepthTexture(std::move(depthTexture))
            , mDepthCamera(depthCamera)
        {
        }

    private:
        void setDefaults(osg::StateSet* stateset) override
        {
            stateset->setTextureAttribute(3, mDepthTexture);
            stateset->addUniform(new osg::Uniform("orthoDepthMap", 3));
            stateset->addUniform(new osg::Uniform("depthSpaceMatrix", mDepthSpaceMatrix));
        }
        void apply(osg::StateSet* stateset, osg::NodeVisitor* /*nv*/) override
        {
            // OpenMGE XE: the depth camera's own matrices, not those of whichever camera is culling the sky. The
            // depth camera culls the sky too (and did so last, so its matrices won), but it now skips frames when it
            // hasn't moved, and the map it drew before still goes with its matrices.
            stateset->getUniform("depthSpaceMatrix")
                ->set(mDepthCamera->getViewMatrix() * mDepthCamera->getProjectionMatrix());
        }

        osg::Matrixf mDepthSpaceMatrix;
        osg::ref_ptr<osg::Texture2D> mDepthTexture;
        osg::ref_ptr<osg::Camera> mDepthCamera;
    };

    class DepthCameraUpdater : public SceneUtil::StateSetUpdater
    {
    public:
        DepthCameraUpdater()
            : mDummyTexture(new osg::Texture2D)
        {
            mDummyTexture->setInternalFormat(GL_RGB);
            mDummyTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            mDummyTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            mDummyTexture->setTextureSize(1, 1);

            Shader::ShaderManager& shaderMgr
                = MWBase::Environment::get().getResourceSystem()->getSceneManager()->getShaderManager();
            mProgram = shaderMgr.getProgram("depthclipped");
        }

    private:
        void setDefaults(osg::StateSet* stateset) override
        {
            stateset->addUniform(new osg::Uniform("projectionMatrix", osg::Matrixf()));
            stateset->setAttributeAndModes(mProgram, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            stateset->setTextureAttribute(0, mDummyTexture);
            stateset->setRenderBinDetails(
                osg::StateSet::OPAQUE_BIN, "RenderBin", osg::StateSet::OVERRIDE_PROTECTED_RENDERBIN_DETAILS);
        }
        void apply(osg::StateSet* stateset, osg::NodeVisitor* nv) override
        {
            osg::Camera* camera = nv->asCullVisitor()->getCurrentCamera();
            stateset->getUniform("projectionMatrix")->set(camera->getProjectionMatrix());
        }

        osg::Matrixf mProjectionMatrix;
        osg::ref_ptr<osg::Texture2D> mDummyTexture;
        osg::ref_ptr<osg::Program> mProgram;
    };
}

namespace MWRender
{
    PrecipitationOccluder::PrecipitationOccluder(
        osg::Group* skyNode, osg::Group* sceneNode, osg::Group* rootNode, osg::Camera* camera)
        : mSkyNode(skyNode)
        , mSceneNode(sceneNode)
        , mRootNode(rootNode)
        , mSceneCamera(camera)
    {
        // OpenMGE XE: a wider area (weather particle occlusion range) gets twice the resolution
        const int rttSize = Settings::shaders().mWeatherParticleOcclusionRange > 0.f ? 512 : 256;

        mDepthTexture = new osg::Texture2D;
        mDepthTexture->setTextureSize(rttSize, rttSize);
        mDepthTexture->setSourceFormat(GL_DEPTH_COMPONENT);
        mDepthTexture->setInternalFormat(GL_DEPTH_COMPONENT24);
        mDepthTexture->setSourceType(GL_UNSIGNED_INT);
        mDepthTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_BORDER);
        mDepthTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_BORDER);
        mDepthTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        mDepthTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        mDepthTexture->setBorderColor(
            SceneUtil::AutoDepth::isReversed() ? osg::Vec4(0, 0, 0, 0) : osg::Vec4(1, 1, 1, 1));

        mCamera = new osg::Camera;
        mCamera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
        mCamera->setRenderOrder(osg::Camera::PRE_RENDER);
        mCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        mCamera->setReferenceFrame(osg::Camera::ABSOLUTE_RF_INHERIT_VIEWPOINT);
        mCamera->setNodeMask(Mask_RenderToTexture);
        // OpenMGE XE: optionally without items and lights (Mask_Object); statics, activators, doors and containers stay
        mCamera->setCullMask(Settings::shaders().mWeatherParticleOcclusionSkipItemsAndLights
                ? (Mask_Scene | Mask_Static)
                : (Mask_Scene | Mask_Object | Mask_Static));
        mCamera->setViewport(0, 0, rttSize, rttSize);
        mCamera->attach(osg::Camera::DEPTH_BUFFER, mDepthTexture);
        mCamera->addChild(mSceneNode);
        mCamera->setSmallFeatureCullingPixelSize(
            Settings::shaders().mWeatherParticleOcclusionSmallFeatureCullingPixelSize);

        SceneUtil::setCameraClearDepth(mCamera);
    }

    void PrecipitationOccluder::update()
    {
        if (!mRange.has_value())
            return;

        osg::Vec3 pos = mSceneCamera->getInverseViewMatrix().getTrans();

        // OpenMGE XE: draw the map again only once the camera has moved to another square of the update distance
        // grid, or a second has passed (objects coming into view); otherwise the map drawn before still holds. The
        // map is one grid step wider, so it still reaches as far round the camera from the square's centre.
        const float step = Settings::shaders().mWeatherParticleOcclusionUpdateDistance;
        float extra = 0.f;
        if (step > 0.f)
        {
            pos = osg::Vec3(std::round(pos.x() / step) * step, std::round(pos.y() / step) * step,
                std::round(pos.z() / step) * step);
            extra = step;
            const auto now = std::chrono::steady_clock::now();
            if (mDrawnCenter && *mDrawnCenter == pos && now - mDrawnAt < std::chrono::seconds(1))
            {
                mCamera->setNodeMask(0);
                return;
            }
            mDrawnCenter = pos;
            mDrawnAt = now;
        }
        mCamera->setNodeMask(Mask_RenderToTexture);

        const float zmin = pos.z() - mRange->z() - Constants::CellSizeInUnits;
        const float zmax = pos.z() + mRange->z() + Constants::CellSizeInUnits;
        const float near = 0;
        const float far = zmax - zmin;

        const float left = -(mRange->x() + extra) / 2;
        const float right = -left;
        const float top = (mRange->y() + extra) / 2;
        const float bottom = -top;

        if (SceneUtil::AutoDepth::isReversed())
        {
            mCamera->setProjectionMatrix(
                SceneUtil::getReversedZProjectionMatrixAsOrtho(left, right, bottom, top, near, far));
        }
        else
        {
            mCamera->setProjectionMatrix(osg::Matrixf::ortho(left, right, bottom, top, near, far));
        }

        mCamera->setViewMatrixAsLookAt(
            osg::Vec3(pos.x(), pos.y(), zmax), osg::Vec3(pos.x(), pos.y(), zmin), osg::Vec3(0, 1, 0));
    }

    void PrecipitationOccluder::enable()
    {
        mEnabled = true;
        mDrawnCenter = std::nullopt;
        mSkyCullCallback = new PrecipitationOcclusionUpdater(mDepthTexture, mCamera.get());
        mSkyNode->addCullCallback(mSkyCullCallback);
        mCamera->setCullCallback(new DepthCameraUpdater);

        mRootNode->removeChild(mCamera);
        mRootNode->addChild(mCamera);
    }

    void PrecipitationOccluder::disable()
    {
        mEnabled = false;
        mSkyNode->removeCullCallback(mSkyCullCallback);
        mCamera->setCullCallback(nullptr);
        mSkyCullCallback = nullptr;

        mRootNode->removeChild(mCamera);
        mRange = std::nullopt;
    }

    void PrecipitationOccluder::updateRange(const osg::Vec3f range)
    {
        assert(range.x() != 0);
        assert(range.y() != 0);
        assert(range.z() != 0);
        const osg::Vec3f margin = { -50, -50, 0 };
        osg::Vec3f newRange = range - margin;
        // OpenMGE XE: at least [Shaders] weather particle occlusion range wide, for post-processing rain
        const float minRange = Settings::shaders().mWeatherParticleOcclusionRange;
        newRange.x() = std::max(newRange.x(), minRange);
        newRange.y() = std::max(newRange.y(), minRange);
        // (this is called every frame while the weather is set; only a new size makes the map be drawn again)
        if (!mRange || *mRange != newRange)
            mDrawnCenter = std::nullopt;
        mRange = newRange;
    }
}
