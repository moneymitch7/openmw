#ifndef OPENMW_COMPONENTS_SCENEUTIL_CLUSTEREDLIGHTING_HPP
#define OPENMW_COMPONENTS_SCENEUTIL_CLUSTEREDLIGHTING_HPP

#include <cstdint>
#include <osg/Vec4f>

namespace SceneUtil
{
    struct Cluster
    {
        osg::Vec4f mMinPoint;
        osg::Vec4f mMaxPoint;
    };

    struct LightGrid
    {
        std::uint32_t mOffset;
        std::uint32_t mCount;
    };

    struct PointLight
    {
        osg::Vec4f mPosition;
        osg::Vec4f mDiffuse;
        osg::Vec4f mAmbient;
        osg::Vec4f mSpecular;
        float mConstant;
        float mLinear;
        float mQuadratic;
        float mRadius;
        // Lamp and lantern shadows (LampShadows, HeldLightShadow): x a lamp's first still face layer + 1, y its first
        // live face layer + 1 (0: none), z the far end of its faces; w 1 for a carried light (its shadows may be drawn)
        osg::Vec4f mShadow;
    };
}

#endif
