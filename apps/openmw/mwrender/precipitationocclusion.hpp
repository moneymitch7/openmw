#ifndef OPENMW_MWRENDER_PRECIPITATIONOCCLUSION_H
#define OPENMW_MWRENDER_PRECIPITATIONOCCLUSION_H

#include <osg/Camera>
#include <osg/Texture2D>

#include <optional>

namespace MWRender
{
    class PrecipitationOccluder
    {
    public:
        PrecipitationOccluder(osg::Group* skyNode, osg::Group* sceneNode, osg::Group* rootNode, osg::Camera* camera);

        void update();

        void enable();

        void disable();

        void updateRange(const osg::Vec3f range);

        // OpenMGE XE: the map handed to post-processing (omw_IsUnderCover)
        bool isActive() const { return mEnabled && mRange.has_value(); }
        osg::Texture2D* getDepthTexture() const { return mDepthTexture.get(); }
        osg::Matrixf getDepthSpaceMatrix() const { return mCamera->getViewMatrix() * mCamera->getProjectionMatrix(); }

    private:
        bool mEnabled = false;
        osg::Group* mSkyNode;
        osg::Group* mSceneNode;
        osg::Group* mRootNode;
        osg::ref_ptr<osg::Callback> mSkyCullCallback;
        osg::ref_ptr<osg::Camera> mCamera;
        osg::ref_ptr<osg::Camera> mSceneCamera;
        osg::ref_ptr<osg::Texture2D> mDepthTexture;
        std::optional<osg::Vec3f> mRange;
    };
}

#endif
