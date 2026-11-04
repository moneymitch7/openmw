#include "mgefog.hpp"

#include <algorithm>
#include <cmath>

#include <osg/StateSet>
#include <osg/Uniform>
#include <osg/Vec2f>
#include <osg/Vec4f>

#include <components/settings/values.hpp>

namespace MWRender
{
    namespace
    {
        constexpr float sCell = 8192.f;
        // MGE XE: exp fog distance scale, and the distance the near fog is fitted to the exp curve at
        constexpr float sExpFogDistScale = 4.f;
        constexpr float sNearFitDist = 1280.f;
        // Morrowind's own draw range in MGE XE, inside which the fog follows the fitted linear range
        constexpr float sNearViewRange = 7168.f;
        // dense weather (fog factor under Cloudy's 0.9): its weights, and how far it pulls the fog start in
        constexpr float sDenseKneeStart = 0.9f;
        constexpr float sDenseKneeWidth = 0.4f;
        constexpr float sLayerKneeWidth = 0.2f;
        constexpr float sFogStartPullIn = 0.65f;

        // [Fog] mge fog start/end cells, or with 'mge fog follows view distance' the end at the viewing distance and
        // the start keeping its share of the way. 0 = MGE XE's own (2 and 5).
        osg::Vec2f getFogRange(float viewDistance)
        {
            float start = Settings::fog().mMgeFogStartCells;
            float end = Settings::fog().mMgeFogEndCells;
            if (start <= 0.f)
                start = 2.f;
            if (end <= 0.f)
                end = 5.f;
            if (Settings::fog().mMgeFogFollowsViewDistance && viewDistance > 0.f)
            {
                const float share = std::clamp(start / end, 0.f, 0.9f);
                end = std::max(viewDistance / sCell, 1.f);
                start = end * share;
            }
            end = std::max(end, start + 0.5f);
            return osg::Vec2f(start, end);
        }

        struct FogRange
        {
            float mExpStart; // units
            float mExpDivisor; // units
            float mEnd; // cells
            float mDense; // dense weather weight
            float mLayer; // height layer weight
        };

        // MGE XE's distantland.cpp adjustFog for a weather's fog factor and offset
        FogRange deriveFogRange(float factor, float offset, const osg::Vec2f& range)
        {
            FogRange fog;
            fog.mEnd = std::max(0.875f, factor * range.y());
            const float lg = std::log(std::max(1.f - 0.25f * offset, 0.05f));
            float start = factor * range.x() + (lg / (1.f + lg)) * fog.mEnd;
            start *= 1.f + (sFogStartPullIn - 1.f) * std::clamp((sDenseKneeStart - factor) / sLayerKneeWidth, 0.f, 1.f);
            fog.mExpStart = start * sCell / sExpFogDistScale;
            fog.mExpDivisor = (fog.mEnd * sCell - fog.mExpStart) / sExpFogDistScale;
            fog.mDense = std::clamp((sDenseKneeStart - factor) / sDenseKneeWidth, 0.f, 1.f);
            fog.mLayer = std::clamp((sDenseKneeStart - factor) / sLayerKneeWidth, 0.f, 1.f);
            return fog;
        }

        float mix(float a, float b, float t)
        {
            return a + (b - a) * t;
        }
    }

    void MgeFogStateUpdater::setSunDirection(const osg::Vec3f& direction)
    {
        mSunDirection = direction;
        if (mSunDirection.normalize() == 0.f)
            mSunDirection = osg::Vec3f(0.f, 0.f, 1.f);
    }

    void MgeFogStateUpdater::setDefaults(osg::StateSet* stateset)
    {
        StateUpdater::setDefaults(stateset);
        stateset->addUniform(new osg::Uniform("mgeFogA", osg::Vec4f(0.f, 1.f, 0.f, 0.f)));
        stateset->addUniform(new osg::Uniform("mgeFogB", osg::Vec4f(0.f, 1.f, 0.f, 0.f)));
        stateset->addUniform(new osg::Uniform("mgeFogState", osg::Vec2f(0.f, 0.f)));
        stateset->addUniform(new osg::Uniform("mgeSkyColor", osg::Vec3f(0.5f, 0.5f, 0.5f)));
        stateset->addUniform(new osg::Uniform("mgeFogColor", osg::Vec3f(0.5f, 0.5f, 0.5f)));
        stateset->addUniform(new osg::Uniform("mgeSunDir", osg::Vec3f(0.f, 0.f, 1.f)));
    }

    void MgeFogStateUpdater::apply(osg::StateSet* stateset, osg::NodeVisitor* nv)
    {
        StateUpdater::apply(stateset, nv);

        // the fog range at both ends of a weather transition, then blended: its dense weather terms ramp over a
        // narrow band of fog factors, which blending the factors instead would squeeze into part of the transition
        const osg::Vec2f range = getFogRange(mViewDistance);
        const FogRange from = deriveFogRange(mWeather.mFogFactor, mWeather.mFogOffset, range);
        const FogRange to = deriveFogRange(mWeather.mNextFogFactor, mWeather.mNextFogOffset, range);
        const float t = std::clamp(mWeather.mTransition, 0.f, 1.f);
        const float expStart = mix(from.mExpStart, to.mExpStart, t);
        const float expDivisor = std::max(mix(from.mExpDivisor, to.mExpDivisor, t), 1.f);
        const float end = mix(from.mEnd, to.mEnd, t);

        // MGE XE's linear near fog, fitted to the exp curve at 1280 units and at the end of Morrowind's draw range
        const float farIntercept = std::min(end * sCell, sNearViewRange);
        const float eNear = std::exp(-(sNearFitDist - expStart) / expDivisor);
        const float eFar = std::exp(-(farIntercept - expStart) / expDivisor);
        float nearStart = 0.f;
        float nearEnd = 1.f;
        if (std::abs(eFar - eNear) > 1e-6f)
        {
            nearStart = sNearFitDist + (farIntercept - sNearFitDist) * (1.f - eNear) / (eFar - eNear);
            nearEnd = sNearFitDist + (farIntercept - sNearFitDist) * (0.f - eNear) / (eFar - eNear);
        }
        if (!std::isfinite(nearStart) || !std::isfinite(nearEnd) || nearEnd <= nearStart)
        {
            nearStart = 0.f;
            nearEnd = farIntercept;
        }

        // through a transition to or from a bad weather, distant land is kept from reading brighter than the sky behind
        // it, and takes that sky's hue
        const float transitionWeight = mWeather.mStormTransition ? 4.f * t * (1.f - t) : 0.f;

        stateset->getUniform("mgeFogA")->set(
            osg::Vec4f(expStart, expDivisor, mix(from.mDense, to.mDense, t), mix(from.mLayer, to.mLayer, t)));
        stateset->getUniform("mgeFogB")
            ->set(osg::Vec4f(nearStart, nearEnd, mWeather.mNiceWeather, transitionWeight));
        stateset->getUniform("mgeFogState")
            ->set(osg::Vec2f(mWeather.mExterior ? 1.f : 0.f, mViewerUnderwater ? 1.f : 0.f));
        stateset->getUniform("mgeSkyColor")->set(mWeather.mSkyColor);
        stateset->getUniform("mgeFogColor")->set(mWeather.mFogColor);

        // MGE XE turns the sun below the horizon at night, for the scattering
        osg::Vec3f sun = mSunDirection;
        if (mNight)
            sun.z() = -std::abs(sun.z());
        stateset->getUniform("mgeSunDir")->set(sun);
    }
}
