#include "mgeweatherpass.hpp"

#include "vismask.hpp"

#include <osg/BlendFunc>
#include <osg/ClampColor>
#include <osg/ColorMask>
#include <osg/Geometry>
#include <osgUtil/CullVisitor>

#include <components/misc/constants.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/shader/shadermanager.hpp>

namespace MWRender
{
    namespace
    {
        /// Lets only the main scene camera into the pass, once per frame (the scene is traversed several times per
        /// frame: shadows, water reflection, the shadow receiving pass).
        class MainViewOncePerFrame
            : public SceneUtil::NodeCallback<MainViewOncePerFrame, osg::Node*, osgUtil::CullVisitor*>
        {
        public:
            void operator()(osg::Node* node, osgUtil::CullVisitor* cv)
            {
                const osg::Camera* camera = cv->getCurrentCamera();
                if (camera == nullptr || camera->getName() != Constants::SceneCamera)
                    return;
                const unsigned int frame = cv->getFrameStamp()->getFrameNumber();
                if (frame == mLastFrame)
                    return;
                mLastFrame = frame;
                traverse(node, cv);
            }

        private:
            unsigned int mLastFrame = ~0u;
        };

        /// One row of the texture, as a clip-space quad (the pass camera has identity matrices).
        osg::ref_ptr<osg::Geometry> createRow(int row)
        {
            const float y0 = -1.f + 2.f * row / MgeWeatherPass::sHeight;
            const float y1 = -1.f + 2.f * (row + 1) / MgeWeatherPass::sHeight;
            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
            vertices->push_back(osg::Vec3f(-1.f, y0, 0.f));
            vertices->push_back(osg::Vec3f(1.f, y0, 0.f));
            vertices->push_back(osg::Vec3f(-1.f, y1, 0.f));
            vertices->push_back(osg::Vec3f(1.f, y1, 0.f));

            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(vertices);
            geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLE_STRIP, 0, 4));
            geometry->setUseDisplayList(false);
            geometry->setUseVertexBufferObjects(true);
            geometry->setCullingActive(false);
            geometry->setName(row == 0 ? "MGE weather verdict (scene)" : "MGE weather verdict (water)");
            return geometry;
        }
    }

    MgeWeatherPass::MgeWeatherPass(Shader::ShaderManager& shaderManager, int textureUnit)
    {
        setName("MGE Weather Pass");
        setCullingActive(false);
        // Like the sky and water RTT cameras: drawn by the main view, but invisible to everything else that walks
        // the scene, above all the activation/crosshair raycasts. Their intersection mask leaves out
        // Mask_RenderToTexture; without it the ray hit the pass's clip-space quad a few units in front of the camera
        // and objects could only be picked up from point-blank range.
        setNodeMask(Mask_RenderToTexture);
        addCullCallback(new MainViewOncePerFrame);

        mTexture = new osg::Texture2D;
        mTexture->setTextureSize(sWidth, sHeight);
        mTexture->setInternalFormat(GL_RGBA32F_ARB);
        mTexture->setSourceFormat(GL_RGBA);
        mTexture->setSourceType(GL_FLOAT);
        mTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        mTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
        mTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        mTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        mTexture->setResizeNonPowerOfTwoHint(false);
        mTexture->setUseHardwareMipMapGeneration(false);

        mCamera = new osg::Camera;
        mCamera->setName("MgeWeatherPassCamera");
        mCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        // Before everything that samples it (the water reflection and the main view).
        mCamera->setRenderOrder(osg::Camera::PRE_RENDER, -1000);
        mCamera->setReferenceFrame(osg::Camera::ABSOLUTE_RF);
        mCamera->setProjectionMatrix(osg::Matrix::identity());
        mCamera->setViewMatrix(osg::Matrix::identity());
        mCamera->setViewport(0, 0, sWidth, sHeight);
        mCamera->setClearMask(0); // every texel is written
        mCamera->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
        mCamera->setCullingActive(false);
        mCamera->setImplicitBufferAttachmentMask(0, 0); // no depth buffer
        mCamera->attach(osg::Camera::COLOR_BUFFER0, mTexture);

        osg::StateSet* stateset = mCamera->getOrCreateStateSet();
        const osg::StateAttribute::GLModeValue off
            = osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED;
        const osg::StateAttribute::GLModeValue on
            = osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED;
        stateset->setMode(GL_DEPTH_TEST, off);
        stateset->setMode(GL_BLEND, off);
        stateset->setMode(GL_CULL_FACE, off);
        stateset->setAttributeAndModes(new osg::ColorMask(true, true, true, true), on);
        // Float values out of [0, 1] (hours, packed weather indices) must reach the texture as they are.
        stateset->setAttribute(new osg::ClampColor(GL_FALSE, GL_FALSE, GL_FALSE), on);
        // Never bind the texture being rendered into; the pass doesn't sample it anyway.
        if (textureUnit >= 0)
        {
            osg::ref_ptr<osg::Texture2D> dummy = new osg::Texture2D;
            dummy->setTextureSize(1, 1);
            dummy->setInternalFormat(GL_RGBA8);
            dummy->setSourceFormat(GL_RGBA);
            dummy->setSourceType(GL_UNSIGNED_BYTE);
            stateset->setTextureAttribute(textureUnit, dummy, on);
        }

        for (int row = 0; row < sHeight; ++row)
        {
            Shader::ShaderManager::DefineMap defines;
            defines["mgeWxWater"] = row == 1 ? "1" : "0";
            osg::ref_ptr<osg::Shader> vertex = shaderManager.getShader("mge_weather.vert", {}, osg::Shader::VERTEX);
            osg::ref_ptr<osg::Shader> fragment
                = shaderManager.getShader("mge_weather.frag", defines, osg::Shader::FRAGMENT);
            osg::ref_ptr<osg::Geometry> geometry = createRow(row);
            if (vertex && fragment)
                geometry->getOrCreateStateSet()->setAttributeAndModes(
                    shaderManager.getProgram(vertex, fragment), on);
            mCamera->addChild(geometry);
        }

        addChild(mCamera);
    }
}
