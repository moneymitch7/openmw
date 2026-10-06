#ifndef OPENMW_COMPONENTS_SCENEUTIL_SHADOWCASTERFILTER_H
#define OPENMW_COMPONENTS_SCENEUTIL_SHADOWCASTERFILTER_H

#include <osg/Callback>
#include <osg/Node>
#include <osg/Vec3f>

namespace SceneUtil::ShadowCasterFilter
{
    /// Leaves objects out of the shadow maps while they look too small from the main camera for their shadow to
    /// matter: a cup across the street casts a shadow of a pixel or two, but drawing it into every shadow map still
    /// costs culling, a draw call and GPU time. Judged by the object's size on screen (seen from the player's camera,
    /// not the shadow map, whose perspective warp makes its own sizes unreliable), so nothing close by loses its
    /// shadow.
    ///
    /// The shadow technique sets the state around culling its shadow casters; the cull callback on each object reads
    /// it. The cull runs on the main thread.
    struct State
    {
        bool mActive = false;
        osg::Vec3f mEye;
        /// Objects whose bounding radius is below this fraction of their distance from the eye are left out.
        float mMinSizeRatio = 0.f;
    };

    namespace Detail
    {
        inline State sState;
    }

    inline const State& current()
    {
        return Detail::sState;
    }

    /// Set while the shadow casters are culled, cleared on destruction.
    class Scope
    {
    public:
        Scope(const osg::Vec3f& eye, float minSizeRatio)
        {
            Detail::sState.mActive = minSizeRatio > 0.f;
            Detail::sState.mEye = eye;
            Detail::sState.mMinSizeRatio = minSizeRatio;
        }
        ~Scope() { Detail::sState.mActive = false; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

    /// Cull callback for objects that may be left out of the shadow maps when small on screen. Does nothing outside the
    /// shadow caster cull.
    class SmallCasterCallback : public osg::Callback
    {
    public:
        bool run(osg::Object* object, osg::Object* data) override
        {
            const State& state = current();
            if (state.mActive)
            {
                if (const osg::Node* node = object->asNode())
                {
                    const osg::BoundingSphere& bound = node->getBound();
                    if (bound.valid()
                        && bound.radius() < (osg::Vec3f(bound.center()) - state.mEye).length() * state.mMinSizeRatio)
                        return false;
                }
            }
            return traverse(object, data);
        }
    };
}

#endif
