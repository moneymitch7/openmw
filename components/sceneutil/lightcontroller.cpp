#include "lightcontroller.hpp"

#include <cmath>

#include <osg/NodeVisitor>

#include <components/sceneutil/lightmanager.hpp>

#include <components/misc/rng.hpp>

namespace SceneUtil
{

    LightController::LightController()
        : mType(LT_Normal)
        , mPhase(0.25f + Misc::Rng::rollClosedProbability() * 0.75f)
        , mBrightness(0.675f)
        , mStartTime(0.0)
        , mLastTime(0.0)
        , mTicksToAdvance(0.f)
        , mFlameSeed(static_cast<unsigned int>(Misc::Rng::rollDice(1 << 30)))
    {
    }

    namespace
    {
        float hashToUnit(unsigned int x)
        {
            x ^= x >> 16;
            x *= 0x7feb352dU;
            x ^= x >> 15;
            x *= 0x846ca68bU;
            x ^= x >> 16;
            return static_cast<float>(x & 0xffffff) / static_cast<float>(0xffffff);
        }

        // Smooth noise, 0 to 1: random values at whole numbers of @a t, eased between.
        float valueNoise(double t, unsigned int seed)
        {
            const double floor = std::floor(t);
            const float f = static_cast<float>(t - floor);
            const auto i = static_cast<unsigned int>(static_cast<long long>(floor));
            const float a = hashToUnit(i * 0x9e3779b9U + seed);
            const float b = hashToUnit((i + 1) * 0x9e3779b9U + seed);
            const float s = f * f * (3.f - 2.f * f);
            return a + (b - a) * s;
        }
    }

    void LightController::setType(LightController::LightType type)
    {
        mType = type;
    }

    void LightController::operator()(SceneUtil::LightSource* node, osg::NodeVisitor* nv)
    {
        double time = nv->getFrameStamp()->getSimulationTime();
        if (mStartTime == 0)
            mStartTime = time;

        // disabled early out, light state needs to be set every frame regardless of change, due to the double buffering
        // if (time == mLastTime)
        //    return;

        SceneUtil::Light* light = node->getLight(nv->getTraversalNumber());

        if (mType == LT_Normal)
        {
            float flame = 1.f;
            if (mFlameFlicker > 0.f)
            {
                // A flame's unsteady glow: a slow sway (about two a second) under a quicker flutter, dimming the light
                // by up to 40% at full strength, never brightening it.
                const double t = time - mStartTime;
                const float sway = valueNoise(t * 2.3, mFlameSeed);
                const float flutter = valueNoise(t * 8.7, mFlameSeed ^ 0x5bd1e995U);
                flame = 1.f - mFlameFlicker * 0.4f * (0.6f * sway + 0.4f * flutter);
            }
            light->setDiffuse(mDiffuseColor * flame);
            light->setSpecular(mSpecularColor * flame);
            traverse(node, nv);
            return;
        }

        // Updating flickering at 15 FPS like vanilla.
        constexpr float updateRate = 15.f;
        mTicksToAdvance
            = static_cast<float>(time - mStartTime - mLastTime) * updateRate * 0.25f + mTicksToAdvance * 0.75f;
        mLastTime = time - mStartTime;

        float speed = (mType == LT_Flicker || mType == LT_Pulse) ? 0.1f : 0.05f;
        if (mBrightness >= mPhase)
            mBrightness -= mTicksToAdvance * speed;
        else
            mBrightness += mTicksToAdvance * speed;

        if (std::abs(mBrightness - mPhase) < speed)
        {
            if (mType == LT_Flicker || mType == LT_FlickerSlow)
                mPhase = 0.25f + Misc::Rng::rollClosedProbability() * 0.75f;
            else // if (mType == LT_Pulse || mType == LT_PulseSlow)
                mPhase = mPhase <= 0.5f ? 1.f : 0.25f;
        }

        const float result = mBrightness * node->getActorFade();

        light->setDiffuse(mDiffuseColor * result);
        light->setSpecular(mSpecularColor * result);

        traverse(node, nv);
    }

    void LightController::setDiffuse(const osg::Vec4f& color)
    {
        mDiffuseColor = color;
    }

    void LightController::setSpecular(const osg::Vec4f& color)
    {
        mSpecularColor = color;
    }

}
