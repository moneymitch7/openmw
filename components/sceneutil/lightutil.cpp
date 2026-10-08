#include "lightutil.hpp"

#include <osg/Group>

#include <osgParticle/ParticleSystem>

#include <components/esm3/loadligh.hpp>
#include <components/fallback/fallback.hpp>
#include <components/sceneutil/lightcommon.hpp>

#include "lightcontroller.hpp"
#include "lightmanager.hpp"
#include "visitor.hpp"

namespace
{
    class CheckEmptyLightVisitor : public osg::NodeVisitor
    {
    public:
        CheckEmptyLightVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Drawable& drawable) override
        {
            if (!mEmpty)
                return;

            if (dynamic_cast<const osgParticle::ParticleSystem*>(&drawable))
                mEmpty = false;
            else
                traverse(drawable);
        }

        void apply(osg::Geometry& geometry) override { mEmpty = false; }

        bool mEmpty = true;
    };
}

namespace SceneUtil
{

    void configureLight(SceneUtil::Light* light, float radius, bool isExterior)
    {
        float quadraticAttenuation = 0.f;
        float linearAttenuation = 0.f;
        float constantAttenuation = 0.f;

        static const bool useConstant = Fallback::Map::getBool("LightAttenuation_UseConstant");
        static const bool useLinear = Fallback::Map::getBool("LightAttenuation_UseLinear");
        static const bool useQuadratic = Fallback::Map::getBool("LightAttenuation_UseQuadratic");
        // User file might provide nonsense values
        // Clamp these settings to prevent badness (e.g. illegal OpenGL calls)
        static const float constantValue = std::max(Fallback::Map::getFloat("LightAttenuation_ConstantValue"), 0.f);
        static const float linearValue = std::max(Fallback::Map::getFloat("LightAttenuation_LinearValue"), 0.f);
        static const float quadraticValue = std::max(Fallback::Map::getFloat("LightAttenuation_QuadraticValue"), 0.f);
        static const float linearRadiusMult
            = std::max(Fallback::Map::getFloat("LightAttenuation_LinearRadiusMult"), 0.f);
        static const float quadraticRadiusMult
            = std::max(Fallback::Map::getFloat("LightAttenuation_QuadraticRadiusMult"), 0.f);
        static const int linearMethod = Fallback::Map::getInt("LightAttenuation_LinearMethod");
        static const int quadraticMethod = Fallback::Map::getInt("LightAttenuation_QuadraticMethod");
        static const bool outQuadInLin = Fallback::Map::getBool("LightAttenuation_OutQuadInLin");

        if (useConstant)
            constantAttenuation = constantValue;

        if (useLinear)
        {
            linearAttenuation = linearMethod == 0 ? linearValue : 0.01f;
            float r = radius * linearRadiusMult;
            if (r > 0.f && (linearMethod == 1 || linearMethod == 2))
                linearAttenuation = linearValue / std::pow(r, static_cast<float>(linearMethod));
        }

        if (useQuadratic && (!outQuadInLin || isExterior))
        {
            quadraticAttenuation = quadraticMethod == 0 ? quadraticValue : 0.01f;
            float r = radius * quadraticRadiusMult;
            if (r > 0.f && (quadraticMethod == 1 || quadraticMethod == 2))
                quadraticAttenuation = quadraticValue / std::pow(r, static_cast<float>(quadraticMethod));
        }

        // If the values are still nonsense, try to at least prevent UB and disable attenuation
        if (constantAttenuation == 0.f && linearAttenuation == 0.f && quadraticAttenuation == 0.f)
            constantAttenuation = 1.f;

        light->setConstantAttenuation(constantAttenuation);
        light->setLinearAttenuation(linearAttenuation);
        light->setQuadraticAttenuation(quadraticAttenuation);
    }

    osg::ref_ptr<LightSource> addLight(osg::Group* node, const SceneUtil::LightCommon& esmLight, unsigned int lightMask,
        bool isExterior, const LightTuning& tuning)
    {
        SceneUtil::FindByNameVisitor visitor("AttachLight");
        node->accept(visitor);

        osg::Group* attachTo = visitor.mFoundNode ? visitor.mFoundNode : node;
        osg::ref_ptr<LightSource> lightSource
            = createLightSource(esmLight, lightMask, isExterior, osg::Vec4f(0, 0, 0, 1), tuning);
        attachTo->addChild(lightSource);

        CheckEmptyLightVisitor emptyVisitor;
        node->accept(emptyVisitor);

        lightSource->setEmpty(emptyVisitor.mEmpty);

        return lightSource;
    }

    namespace
    {
        struct TunedLight
        {
            float mRadius;
            osg::Vec4f mDiffuse;
            osg::Vec4f mSpecular;
        };

        // Radius, attenuation and colours of a light from its game data and tuning. Sets the attenuation of light.
        TunedLight tuneLight(
            Light& light, const SceneUtil::LightCommon& esmLight, bool isExterior, const LightTuning& tuning)
        {
            // The minimum scene light radius is 16 in Morrowind
            const float radius = std::max(esmLight.mRadius, 16.f);
            configureLight(&light, radius, isExterior);

            float colourScale = tuning.mBrightness;
            if (tuning.mSoftness > 0.f)
            {
                // The attenuation's distance terms at the light's radius and at the third of it where it reaches its
                // full colour (with the usual settings). Adding a share of the former as a constant term caps the
                // brightness close to the light; the colour is raised to keep the brightness at the pivot as it was,
                // so the light gets flatter rather than just dimmer.
                const float linear = light.getLinearAttenuation();
                const float quadratic = light.getQuadraticAttenuation();
                const float atRadius = linear * radius + quadratic * radius * radius;
                const float pivot = radius / 3.f;
                const float atPivot = light.getConstantAttenuation() + linear * pivot + quadratic * pivot * pivot;
                if (atRadius > 0.f && atPivot > 0.f)
                {
                    const float constant = tuning.mSoftness * atRadius;
                    light.setConstantAttenuation(light.getConstantAttenuation() + constant);
                    colourScale *= (atPivot + constant) / atPivot;
                }
            }

            TunedLight result;
            result.mRadius = radius * tuning.mReach;
            result.mDiffuse = esmLight.mColor;
            result.mSpecular = esmLight.mColor; // ESM format doesn't provide specular
            if (esmLight.mNegative)
            {
                result.mDiffuse *= -1;
                result.mDiffuse.a() = 1;
                // Using specular lighting for negative lights is unreasonable
                result.mSpecular = osg::Vec4f();
            }
            for (int i = 0; i < 3; ++i)
            {
                result.mDiffuse[i] *= colourScale;
                result.mSpecular[i] *= colourScale;
            }
            return result;
        }

        LightController* findLightController(LightSource& lightSource)
        {
            for (osg::Callback* callback = lightSource.getUpdateCallback(); callback != nullptr;
                 callback = callback->getNestedCallback())
            {
                if (auto* controller = dynamic_cast<LightController*>(callback))
                    return controller;
            }
            return nullptr;
        }
    }

    osg::ref_ptr<LightSource> createLightSource(const SceneUtil::LightCommon& esmLight, unsigned int lightMask,
        bool isExterior, const osg::Vec4f& ambient, const LightTuning& tuning)
    {
        osg::ref_ptr<SceneUtil::LightSource> lightSource(new SceneUtil::LightSource);
        osg::ref_ptr<SceneUtil::Light> light(new SceneUtil::Light);
        lightSource->setNodeMask(lightMask);

        const TunedLight tuned = tuneLight(*light, esmLight, isExterior, tuning);
        lightSource->setRadius(tuned.mRadius);
        lightSource->setBounce(tuning.mBounce);

        light->setDiffuse(tuned.mDiffuse);
        light->setAmbient(ambient);
        light->setSpecular(tuned.mSpecular);

        lightSource->setLight(light);

        osg::ref_ptr<SceneUtil::LightController> ctrl(new SceneUtil::LightController);
        ctrl->setDiffuse(light->getDiffuse());
        ctrl->setSpecular(light->getSpecular());
        if (esmLight.mFlicker)
            ctrl->setType(SceneUtil::LightController::LT_Flicker);
        if (esmLight.mFlickerSlow)
            ctrl->setType(SceneUtil::LightController::LT_FlickerSlow);
        if (esmLight.mPulse)
            ctrl->setType(SceneUtil::LightController::LT_Pulse);
        if (esmLight.mPulseSlow)
            ctrl->setType(SceneUtil::LightController::LT_PulseSlow);

        lightSource->addUpdateCallback(ctrl);

        return lightSource;
    }

    void retuneLightSource(
        LightSource& lightSource, const SceneUtil::LightCommon& esmLight, bool isExterior, const LightTuning& tuning)
    {
        // Both of the light's frame buffers: the light controller sets the colours of the frame being prepared every
        // frame, but nothing else sets the attenuation.
        TunedLight tuned{};
        for (std::size_t frame = 0; frame < 2; ++frame)
        {
            Light& light = *lightSource.getLight(frame);
            tuned = tuneLight(light, esmLight, isExterior, tuning);
            light.setDiffuse(tuned.mDiffuse);
            light.setSpecular(tuned.mSpecular);
        }
        lightSource.setRadius(tuned.mRadius);
        lightSource.setBounce(tuning.mBounce);

        if (LightController* controller = findLightController(lightSource))
        {
            controller->setDiffuse(tuned.mDiffuse);
            controller->setSpecular(tuned.mSpecular);
        }
    }
}
