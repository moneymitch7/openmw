#ifndef COMPONENTS_SCENEUTIL_SHADOW_H
#define COMPONENTS_SCENEUTIL_SHADOW_H

#include <components/shader/shadermanager.hpp>

namespace osg
{
    class StateSet;
    class Group;
}

namespace osgShadow
{
    class ShadowSettings;
    class ShadowedScene;
}

namespace Settings
{
    struct ShadowsCategory;
}

namespace SceneUtil
{
    class MWShadowTechnique;
    class ShadowManager
    {
    public:
        static Shader::ShaderManager::DefineMap getShadowsDisabledDefines();

        static const ShadowManager& instance();

        explicit ShadowManager(osg::ref_ptr<osg::Group> sceneRoot, osg::ref_ptr<osg::Group> rootNode,
            unsigned int outdoorShadowCastingMask, unsigned int indoorShadowCastingMask, unsigned int worldMask,
            const Settings::ShadowsCategory& settings, Shader::ShaderManager& shaderManager);
        ~ShadowManager();

        void setupShadowSettings(const Settings::ShadowsCategory& settings, Shader::ShaderManager& shaderManager);
        void setOutdoorShadowCastingMask(unsigned int mask);
        void setIndoorShadowCastingMask(unsigned int mask);
        /// OpenMGE XE [Shadows] static shadow cache: which casters never move, and which do
        void setShadowCacheMasks(unsigned int staticMask, unsigned int dynamicMask);
        /// OpenMGE XE: the node mask [Shadows] actor shadow maps applies to (NPCs and creatures)
        void setActorMask(unsigned int mask);

        void setupShaders(Shader::ShaderManager& shaderManager) const;

        void disableShadowsForStateSet(osg::StateSet& stateset) const;

        Shader::ShaderManager::DefineMap getShadowDefines(const Settings::ShadowsCategory& settings) const;

        void enableIndoorMode(const Settings::ShadowsCategory& settings);

        void enableOutdoorMode();

        void updateCastingMasks(unsigned int outdoorMask, unsigned int indoorMask);

        /// Stops drawing outdoor sun shadows while the sun shadow strength is zero (at night with [Shadows] night
        /// shadows off), so their cull and draw cost goes too. Indoor shadows are unaffected.
        void setSunShadowsSuppressed(bool suppressed);

    protected:
        static ShadowManager* sInstance;

        bool mEnableShadows;

        osg::ref_ptr<osgShadow::ShadowedScene> mShadowedScene;
        osg::ref_ptr<osgShadow::ShadowSettings> mShadowSettings;
        osg::ref_ptr<MWShadowTechnique> mShadowTechnique;

        unsigned int mOutdoorShadowCastingMask;
        unsigned int mIndoorShadowCastingMask;

        bool mIndoorMode = false;
        bool mSunShadowsSuppressed = false;
        unsigned int mActorMask = 0;
        unsigned int mActorShadowMaps = 0;
    };
}

#endif // COMPONENTS_SCENEUTIL_SHADOW_H
