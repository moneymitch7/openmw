#include "lightmanager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

#include <osg/ComputeBoundsVisitor>
#include <osg/Geometry>
#include <osg/TriangleIndexFunctor>
#include <osgUtil/CullVisitor>

#include <components/debug/debuglog.hpp>
#include <components/misc/constants.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/util.hpp>
#include <components/shader/shadermanager.hpp>

#include "cullprofile.hpp"
#include "memorybarrier.hpp"
#include "morphgeometry.hpp"
#include "riggeometry.hpp"
#include "riggeometryosgaextension.hpp"

namespace
{
    void configurePosition(osg::Matrixf& mat, const osg::Vec4& pos)
    {
        mat(0, 0) = pos.x();
        mat(0, 1) = pos.y();
        mat(0, 2) = pos.z();
    }

    void configureAmbient(osg::Matrixf& mat, const osg::Vec4& color)
    {
        mat(1, 0) = color.r();
        mat(1, 1) = color.g();
        mat(1, 2) = color.b();
    }

    void configureDiffuse(osg::Matrixf& mat, const osg::Vec4& color)
    {
        mat(2, 0) = color.r();
        mat(2, 1) = color.g();
        mat(2, 2) = color.b();
    }

    void configureSpecular(osg::Matrixf& mat, const osg::Vec4& color)
    {
        mat(3, 0) = color.r();
        mat(3, 1) = color.g();
        mat(3, 2) = color.b();
        mat(3, 3) = color.a();
    }

    void configureAttenuation(osg::Matrixf& mat, float c, float l, float q, float r)
    {
        mat(0, 3) = c;
        mat(1, 3) = l;
        mat(2, 3) = q;
        mat(3, 3) = r;
    }
}

namespace SceneUtil
{
    namespace
    {
        // lightShade in lib/light/bindings.glsl: a uvec4 per 32 lights.
        osg::ref_ptr<osg::Uniform> makeBlockedLightsUniform(const LightManager::BlockedLightsMask& mask)
        {
            constexpr unsigned int vectors = LightManager::sMaxBlockableLights / 32;
            osg::ref_ptr<osg::Uniform> uniform
                = new osg::Uniform(osg::Uniform::UNSIGNED_INT_VEC4, "lightShade", vectors);
            for (unsigned int i = 0; i < vectors; ++i)
                uniform->setElement(i, mask[i * 4], mask[i * 4 + 1], mask[i * 4 + 2], mask[i * 4 + 3]);
            return uniform;
        }
    }

    static int sLightId = 0;

    void configureStateSetSunOverride(const Light* light, osg::StateSet* stateset, int mode)
    {
        stateset->addUniform(new osg::Uniform("sun.position", light->getPosition()), mode);
        stateset->addUniform(new osg::Uniform("sun.diffuse", light->getDiffuse()), mode);
        stateset->addUniform(new osg::Uniform("sun.ambient", light->getAmbient()), mode);
        stateset->addUniform(new osg::Uniform("sun.specular", light->getSpecular()), mode);
    }

    void configureSunAmbientOverride(const osg::Vec4f& ambient, osg::StateSet* stateset)
    {
        stateset->getOrCreateUniform("sun.ambient", osg::Uniform::FLOAT_VEC4)->set(ambient);
    }

    LightManager* findLightManager(const osg::NodePath& path)
    {
        for (size_t i = 0; i < path.size(); ++i)
        {
            if (LightManager* lightManager = dynamic_cast<LightManager*>(path[i]))
                return lightManager;
        }
        return nullptr;
    }

    // Set on a LightSource. Adds the light source to its light manager for the current frame.
    // This allows us to keep track of the current lights in the scene graph without tying creation & destruction to the
    // manager.
    class CollectLightCallback : public NodeCallback<CollectLightCallback>
    {
    public:
        CollectLightCallback()
            : mLightManager(nullptr)
        {
        }

        CollectLightCallback(const CollectLightCallback& copy, const osg::CopyOp& copyop)
            : NodeCallback<CollectLightCallback>(copy, copyop)
            , mLightManager(nullptr)
        {
        }

        META_Object(SceneUtil, CollectLightCallback)

        void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            if (!mLightManager)
            {
                mLightManager = findLightManager(nv->getNodePath());

                if (!mLightManager)
                    throw std::runtime_error("can't find parent LightManager");
            }

            mLightManager->addLight(
                static_cast<LightSource*>(node), osg::computeLocalToWorld(nv->getNodePath()), nv->getTraversalNumber());

            traverse(node, nv);
        }

    private:
        LightManager* mLightManager;
    };

    // Set on a LightManager. Clears the data from the previous frame.
    class LightManagerUpdateCallback : public SceneUtil::NodeCallback<LightManagerUpdateCallback>
    {
    public:
        void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            LightManager* lightManager = static_cast<LightManager*>(node);
            lightManager->update(nv->getTraversalNumber());

            traverse(node, nv);
        }
    };

    LightManagerCullCallback::LightManagerCullCallback(const LightSettings& settings)
        : mGridSizeX(settings.mClusteredGridSize.x())
        , mGridSizeY(settings.mClusteredGridSize.y())
        , mGridSizeZ(settings.mClusteredGridSize.z())
        , mNumClusters(mGridSizeX * mGridSizeY * mGridSizeZ)
        , mWorkGroupSize(settings.mClusteredWorkGroupSize)
    {
    }

    void LightManagerCullCallback::operator()(LightManager* node, osgUtil::CullVisitor* cv)
    {
        if (!(cv->getTraversalMask() & node->getLightingMask()))
        {
            traverse(node, cv);
            return;
        }

        const size_t frame = cv->getTraversalNumber();
        const size_t frameId = frame % 2;

        auto& cache = mCache[cv->getCurrentCamera()];

        float clusterFar = node->getPointLightFadeEnd();

        // TODO: We will need to rethink this once distant lights are a thing.
        if (clusterFar == 0.f)
            clusterFar = Constants::CellSizeInUnits;

        bool rebuildCluster = false;

        if (node->getClusteredLighting())
        {
            auto& clusterNode = cache.mClusterComputeNode[frameId];
            auto& cullNode = cache.mCullComputeNode[frameId];

            if (!clusterNode || !cullNode)
            {
                if (!node->getResourceSystem())
                    throw std::runtime_error("Resource system must be defined");

                auto& shaderManager = node->getResourceSystem()->getSceneManager()->getShaderManager();

                clusterNode = new osg::DispatchCompute;
                clusterNode->setCullingActive(false);
                clusterNode->getOrCreateStateSet()->addUniform(
                    new osg::Uniform("inverseProjectionMatrix", osg::Matrixf{}));
                clusterNode->getOrCreateStateSet()->setAttribute(
                    shaderManager.getProgram(
                        nullptr, shaderManager.getShader("core/lighting/cluster.comp", {}, osg::Shader::COMPUTE)),
                    osg::StateAttribute::ON);
                clusterNode->setDrawCallback(new MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT));

                cullNode = new osg::DispatchCompute;
                cullNode->setCullingActive(false);
                cullNode->getOrCreateStateSet()->setAttribute(
                    shaderManager.getProgram(nullptr,
                        shaderManager.getShader("core/lighting/cull.comp",
                            {
                                { "workGroupSize", std::to_string(mWorkGroupSize) },
                                { "maxLightsPerCluster", std::to_string(mMaxLightsPerCluster) },
                            },
                            osg::Shader::COMPUTE)),
                    osg::StateAttribute::ON);

                cullNode->setComputeGroups(mNumClusters, 1, 1);
                cullNode->setDrawCallback(new MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT));
            }

            // Ensure we rebuild the cluster grid only when the projection matrix changes
            if (cache.mClusterFar != clusterFar || cache.mProjection[frameId] != *cv->getProjectionMatrix())
                rebuildCluster = true;

            cache.mClusterFar = clusterFar;
            cache.mProjection[frameId] = *cv->getProjectionMatrix();

            if (rebuildCluster)
            {
                // When reverse-z is enabled ensure we pass in the "unreversed" projection matrix.
                // This just makes all the maths constant and easier to deal with in general.
                // We also work with a projection matrix with a far plane detached from the current camera.
                double fovy, aspectRatio, near, _;
                cache.mProjection[frameId].getPerspective(fovy, aspectRatio, near, _);

                const osg::Matrixd projection = osg::Matrixd::perspective(fovy, aspectRatio, near, clusterFar);

                clusterNode->setComputeGroups(mGridSizeX, mGridSizeY, mGridSizeZ);
                clusterNode->getStateSet()
                    ->getUniform("inverseProjectionMatrix")
                    ->set(osg::Matrixf::inverse(projection));
            }
        }

        auto& stateset = cache.mStateSet[frameId];

        if (!stateset)
        {
            stateset = new osg::StateSet;
            stateset->addUniform(new osg::Uniform("sun.position", osg::Vec4f{}));
            stateset->addUniform(new osg::Uniform("sun.diffuse", osg::Vec4f{}));
            stateset->addUniform(new osg::Uniform("sun.ambient", osg::Vec4f{}));
            stateset->addUniform(new osg::Uniform("sun.specular", osg::Vec4f{}));

            if (node->getClusteredLighting())
            {
                stateset->addUniform(new osg::Uniform("clusterFar", clusterFar));
                stateset->addUniform(new osg::Uniform("gridSize",
                    osg::Vec3f(static_cast<float>(mGridSizeX), static_cast<float>(mGridSizeY),
                        static_cast<float>(mGridSizeZ))));

                const int maxLightIndices = mMaxLightsPerCluster * mNumClusters;

                osg::ref_ptr<osg::UByteArray> clusterData = new osg::UByteArray(sizeof(Cluster) * mNumClusters);
                clusterData->setBufferObject(new osg::ShaderStorageBufferObject);
                cache.mClusterSSBB
                    = new osg::ShaderStorageBufferBinding(1, clusterData, 0, clusterData->getTotalDataSize());

                for (size_t i = 0; i < cache.mPointLightSSBB.size(); ++i)
                {
                    cache.mGPULights[i] = new osg::BufferTemplate<std::vector<PointLight>>();
                    cache.mGPULights[i]->setBufferObject(new osg::ShaderStorageBufferObject);

                    cache.mPointLightSSBB[i] = new osg::ShaderStorageBufferBinding(
                        2, cache.mGPULights[i], 0, cache.mGPULights[i]->getTotalDataSize());

                    osg::ref_ptr<osg::UIntArray> gridData = new osg::UIntArray(mNumClusters * 2);
                    gridData->setBufferObject(new osg::ShaderStorageBufferObject);
                    cache.mLightGridSSBB[i]
                        = new osg::ShaderStorageBufferBinding(3, gridData, 0, gridData->getTotalDataSize());

                    osg::ref_ptr<osg::UIntArray> indexData = new osg::UIntArray(maxLightIndices);
                    indexData->setBufferObject(new osg::ShaderStorageBufferObject);
                    cache.mLightIndexListSSBB[i]
                        = new osg::ShaderStorageBufferBinding(4, indexData, 0, indexData->getTotalDataSize());

                    osg::ref_ptr<osg::UIntArray> counterData = new osg::UIntArray(1);
                    counterData->setBufferObject(new osg::ShaderStorageBufferObject);
                    cache.mLightIndexCounterSSBB[i]
                        = new osg::ShaderStorageBufferBinding(5, counterData, 0, counterData->getTotalDataSize());
                }
            }
        }

        if (frame != cache.mLastFrameNumber)
        {
            cache.mLastFrameNumber = frame;

            const auto& sun = node->getSunlight();

            // Don't use Camera::getViewMatrix, that one might be relative to another camera!
            const osg::RefMatrix* viewMatrix = cv->getCurrentRenderStage()->getInitialViewMatrix();

            stateset->getUniform("sun.position")->set(sun->getPosition() * (*viewMatrix));
            stateset->getUniform("sun.diffuse")->set(sun->getDiffuse());
            stateset->getUniform("sun.ambient")->set(sun->getAmbient());
            stateset->getUniform("sun.specular")->set(sun->getSpecular());

            if (node->getClusteredLighting())
            {
                if (rebuildCluster)
                {
                    stateset->getUniform("clusterFar")->set(clusterFar);
                    stateset->getUniform("gridSize")
                        ->set(osg::Vec3f(static_cast<float>(mGridSizeX), static_cast<float>(mGridSizeY),
                            static_cast<float>(mGridSizeZ)));
                }

                stateset->setAttribute(cache.mPointLightSSBB[frameId]);
                stateset->setAttribute(cache.mClusterSSBB);
                stateset->setAttribute(cache.mLightGridSSBB[frameId]);
                stateset->setAttribute(cache.mLightIndexListSSBB[frameId]);
                stateset->setAttribute(cache.mLightIndexCounterSSBB[frameId]);

                cache.mGPULights[frameId]->getData().clear();

                for (const auto& bound : node->getLightsInViewSpace(cv, viewMatrix, frame))
                {
                    if (bound.mCulled)
                        continue;

                    const auto& light = bound.mLightSource->getLight(frame);
                    auto gpuLight = PointLight{
                        .mPosition = light->getPosition() * (*viewMatrix),
                        .mDiffuse = light->getDiffuse(),
                        .mAmbient = light->getAmbient(),
                        .mSpecular = light->getSpecular(),
                        .mConstant = light->getConstantAttenuation(),
                        .mLinear = light->getLinearAttenuation(),
                        .mQuadratic = light->getQuadraticAttenuation(),
                        .mRadius = bound.mLightSource->getRadius() * node->getPointLightRadiusMultiplier(),
                    };

                    // w carries the light's own light bounce (negative: the scene's), see lib/light/util.glsl
                    gpuLight.mPosition.w() = bound.mLightSource->getBounce();
                    bound.mGpuIndex = static_cast<int>(cache.mGPULights[frameId]->getData().size());
                    cache.mGPULights[frameId]->getData().push_back(gpuLight);
                }

                // Always add a dummy light, SSBO can't have zero size
                auto& lights = cache.mGPULights[frameId]->getData();
                if (lights.empty())
                    lights.emplace_back();

                cache.mPointLightSSBB[frameId]->setSize(cache.mGPULights[frameId]->getTotalDataSize());
                cache.mGPULights[frameId]->dirty();

                static_cast<osg::UIntArray*>(cache.mLightIndexCounterSSBB[frameId]->getBufferData())->at(0) = 0;
                static_cast<osg::UIntArray*>(cache.mLightIndexCounterSSBB[frameId]->getBufferData())->dirty();
            }
        }

        cv->pushStateSet(stateset);
        if (rebuildCluster)
            cache.mClusterComputeNode[frameId]->accept(*cv);
        if (node->getClusteredLighting())
            cache.mCullComputeNode[frameId]->accept(*cv);
        traverse(node, cv);
        cv->popStateSet();

        if (node->getPPLightsBuffer() && cv->getCurrentCamera()->getName() == Constants::SceneCamera)
            node->getPPLightsBuffer()->updateCount(frame);
    }

    LightManager::LightManager(const LightSettings& settings, Resource::ResourceSystem* resourceSystem)
        : mResourceSystem(resourceSystem)
        , mLightingMask(~0u)
        , mSun(nullptr)
        , mPointLightRadiusMultiplier(1.f)
        , mPointLightFadeEnd(0.f)
        , mPointLightFadeStart(0.f)
    {
        osg::GLExtensions* exts = SceneUtil::glExtensionsReady() ? &SceneUtil::getGLExtensions() : nullptr;
        // In theory the two extensions should allow us to use SSBO and std430 layout in version 120
        //  1. GL_ARB_shader_storage_buffer_object
        //  2. GL_ARB_shading_language_420pack
        // However, this is not the case in practice and is known to be broken on at least Mesa drivers.
        bool supportsSSBO = exts && static_cast<int>(exts->glslLanguageVersion * 100) >= 430;

        mSupportsClustered = supportsSSBO;

        setUpdateCallback(new LightManagerUpdateCallback);

        mCullCallback = new LightManagerCullCallback(settings);
        addCullCallback(mCullCallback);

        static bool hasLoggedWarnings = false;

        if (settings.mClusteredLighting && !hasLoggedWarnings)
        {
            if (!supportsSSBO)
                Log(Debug::Warning) << "GLSL 430 or higher not supported: disabling clustered lighting";
            hasLoggedWarnings = true;
        }

        if (settings.mClusteredLighting && mSupportsClustered)
            initClustered();
        else
            initPerObjectUniform(settings.mMaxLights);

        getOrCreateStateSet()->addUniform(new osg::Uniform("PointLightCount", 0));
        // No light hidden unless a lit object's own state says otherwise.
        getOrCreateStateSet()->addUniform(makeBlockedLightsUniform(BlockedLightsMask{}));
        // Only lamps light their own model from inside (see uSelfLitRange in lib/light/util.glsl).
        getOrCreateStateSet()->addUniform(new osg::Uniform("uSelfLitRange", 0.f));

        updateSettings(settings.mLightRadiusMultiplier, settings.mMaximumLightDistance, settings.mLightFadeStart);
    }

    LightManager::LightManager(const LightManager& copy, const osg::CopyOp& copyop)
        : osg::Group(copy, copyop)
        , mResourceSystem(copy.mResourceSystem)
        , mLightingMask(copy.mLightingMask)
        , mSun(copy.mSun)
        , mClusteredLighting(copy.mClusteredLighting)
        , mPointLightRadiusMultiplier(copy.mPointLightRadiusMultiplier)
        , mPointLightFadeEnd(copy.mPointLightFadeEnd)
        , mPointLightFadeStart(copy.mPointLightFadeStart)
        , mMaxLights(copy.mMaxLights)
        , mSupportsClustered(copy.mSupportsClustered)
        , mPPLightBuffer(copy.mPPLightBuffer)
    {
    }

    bool LightManager::getClusteredLighting() const
    {
        return mClusteredLighting;
    }

    void LightManager::setLightOcclusion(LightOcclusionTest* test, bool enabled, float msPerFrame, float maxObjectRadius)
    {
        mOcclusionTest = test;
        mOcclusionEnabled = enabled && test != nullptr;
        mOcclusionRetestBudget = 1000.0 * msPerFrame;
        mOcclusionNewBudget = 4000.0 * msPerFrame;
        mOcclusionMaxObjectRadius = maxObjectRadius;
    }

    namespace
    {
        // A cached result stands while neither end has moved further than this (world units)...
        constexpr float sOcclusionMoveTolerance2 = 24.f * 24.f;
        // ...and is retested after this many frames anyway (doors open and close), when there is time for it.
        constexpr size_t sOcclusionRefreshFrames = 180;
        // Results not used for this long are dropped (about two minutes): coming back to a room soon after finds them
        // still known, instead of the room lighting up until it has been tested again.
        constexpr size_t sOcclusionForgetFrames = 7200;
        // How often what light occlusion did is written to the log.
        constexpr size_t sOcclusionLogFrames = 600;
    }

    namespace
    {
        // How long a light takes to fade in or out when what it reaches changes (seconds).
        constexpr double sOcclusionFadeTime = 0.5;

        bool segmentHitsBox(const osg::Vec3f& from, const osg::Vec3f& to, const osg::BoundingBox& box)
        {
            float enter = 0.f;
            float leave = 1.f;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float delta = to[axis] - from[axis];
                if (std::abs(delta) < 1e-6f)
                {
                    if (from[axis] < box._min[axis] || from[axis] > box._max[axis])
                        return false;
                    continue;
                }
                float t0 = (box._min[axis] - from[axis]) / delta;
                float t1 = (box._max[axis] - from[axis]) / delta;
                if (t0 > t1)
                    std::swap(t0, t1);
                enter = std::max(enter, t0);
                leave = std::min(leave, t1);
                if (enter > leave)
                    return false;
            }
            return true;
        }

        float fadedVisibility(const LightManager::OcclusionCacheEntry& entry, double time)
        {
            const double t = std::clamp((time - entry.mFadeStart) / sOcclusionFadeTime, 0.0, 1.0);
            const float eased = static_cast<float>(t * t * (3.0 - 2.0 * t));
            return entry.mFadeFrom + (entry.mVisible - entry.mFadeFrom) * eased;
        }
    }

    float LightManager::testLightVisibility(const osg::Vec3f& lightPos, const osg::Vec3f& objectPos, float objectRadius)
    {
        // A small object: its centre and six points halfway out. The share of them the light reaches.
        const float r = objectRadius * 0.5f;
        const osg::Vec3f offsets[]
            = { { 0, 0, 0 }, { 0, 0, r }, { r, 0, 0 }, { -r, 0, 0 }, { 0, r, 0 }, { 0, -r, 0 }, { 0, 0, -r } };
        // Most lights reach the centre and the point facing them: then the light reaches it all.
        osg::Vec3f toLight = lightPos - objectPos;
        toLight.normalize();
        mOcclusionRaysUsed += 2;
        const bool centre = !mOcclusionTest->isBlocked(lightPos, objectPos);
        const bool facing = !mOcclusionTest->isBlocked(lightPos, objectPos + toLight * r);
        if (centre && facing)
            return 1.f;
        unsigned int reached = 0;
        for (const osg::Vec3f& offset : offsets)
        {
            ++mOcclusionRaysUsed;
            if (!mOcclusionTest->isBlocked(lightPos, objectPos + offset))
                ++reached;
        }
        return static_cast<float>(reached) / static_cast<float>(std::size(offsets));
    }

    float LightManager::testLightVisibilityFromBox(const osg::Vec3f& lightPos, const osg::BoundingBox& box)
    {
        // A light inside the object's box (a lamp in a room part, a candle on a table) always reaches it.
        if (box.contains(lightPos))
            return 1.f;

        // Points on the sides of the box facing the light. The rays end this far short of them: a floor or wall is
        // built of tiles lying level with each other, and a ray to the very edge of one tile, or to a point a little
        // inside it, grazes or crosses the next tile along and found the light hidden from a floor right below it.
        // Ending short of the side facing the light, the ray never reaches the object itself either.
        constexpr float shortBy = 8.f;
        const auto visible = [&](const osg::Vec3f& point) {
            osg::Vec3f toLight = lightPos - point;
            const float length = toLight.normalize();
            ++mOcclusionRaysUsed;
            return !mOcclusionTest->isBlocked(lightPos, point + toLight * std::min(shortBy, 0.5f * length));
        };

        // The part of the object nearest the light, and the middle of each side facing the light.
        osg::Vec3f nearest;
        for (int axis = 0; axis < 3; ++axis)
            nearest[axis] = std::clamp(lightPos[axis], box._min[axis], box._max[axis]);
        unsigned int tested = 1;
        unsigned int reached = visible(nearest) ? 1 : 0;

        const osg::Vec3f center = box.center();
        int widest = -1;
        float widestArea = 0.f;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (lightPos[axis] >= box._min[axis] && lightPos[axis] <= box._max[axis])
                continue;
            osg::Vec3f faceCenter = center;
            faceCenter[axis] = lightPos[axis] < box._min[axis] ? box._min[axis] : box._max[axis];
            ++tested;
            if (visible(faceCenter))
                ++reached;
            // the side the light sees the most of: its area seen from the light
            const int u = (axis + 1) % 3;
            const int v = (axis + 2) % 3;
            osg::Vec3f toFace = faceCenter - lightPos;
            const float distance = toFace.normalize();
            const float area = (box._max[u] - box._min[u]) * (box._max[v] - box._min[v]) * std::abs(toFace[axis])
                / std::max(distance * distance, 1.f);
            if (area > widestArea)
            {
                widestArea = area;
                widest = axis;
            }
        }

        // All reached or none: the light reaches all of the object or none of it (a room's own walls and floor, the
        // room beyond a wall). Some: part of the object sees the light (a floor through a doorway), so four more
        // points across the side the light sees most tell how much.
        if (reached == 0 || reached == tested || widest < 0)
            return static_cast<float>(reached) / static_cast<float>(tested);
        const int u = (widest + 1) % 3;
        const int v = (widest + 2) % 3;
        for (const float fu : { 0.25f, 0.75f })
            for (const float fv : { 0.25f, 0.75f })
            {
                osg::Vec3f point;
                point[widest] = lightPos[widest] < box._min[widest] ? box._min[widest] : box._max[widest];
                point[u] = box._min[u] + fu * (box._max[u] - box._min[u]);
                point[v] = box._min[v] + fv * (box._max[v] - box._min[v]);
                ++tested;
                if (visible(point))
                    ++reached;
            }
        return static_cast<float>(reached) / static_cast<float>(tested);
    }

    float LightManager::testLightVisibilityFromSurface(const osg::Vec3f& lightPos, const SurfaceSamples& samples)
    {
        // Points on the object's own surfaces, each nudged off the surface into the open: the share of those facing
        // the light that it reaches. Surfaces turned away from it (the outside of a tunnel's walls, seen from the room
        // next door) take no light from it whatever lies between, so they don't count. Nothing between the light and
        // a point facing it is left out, the object itself included: a tunnel's ceiling hides a lantern above from
        // its steps. The points sit well off the surface: collision shapes are simpler than the meshes and stand up
        // to 30 units in front of them (a ramp over a stair's treads, a wall's flat plane over its mouldings), and a
        // point behind its own object's collision would see nothing.
        constexpr float offset = 40.f;
        unsigned int facing = 0;
        unsigned int reached = 0;
        for (const SurfaceSample& sample : samples)
        {
            osg::Vec3f toLight = lightPos - sample.mPosition;
            const float distance = toLight.normalize();
            if (distance < offset)
                return 1.f;
            if (toLight * sample.mNormal <= 0.05f)
                continue;
            ++facing;
            ++mOcclusionRaysUsed;
            if (!mOcclusionTest->isBlockedToSurface(lightPos, sample.mPosition + sample.mNormal * offset))
                ++reached;
        }
        if (facing > 0)
            return static_cast<float>(reached) / static_cast<float>(facing);
        // Every surface turned away (a sign, a banner seen from behind, two-sided ones among them): judged by what the
        // light reaches of the points themselves.
        for (const SurfaceSample& sample : samples)
        {
            osg::Vec3f toLight = lightPos - sample.mPosition;
            toLight.normalize();
            ++facing;
            ++mOcclusionRaysUsed;
            if (!mOcclusionTest->isBlockedToSurface(lightPos, sample.mPosition + toLight * offset))
                ++reached;
        }
        return facing > 0 ? static_cast<float>(reached) / static_cast<float>(facing) : 1.f;
    }

    bool LightManager::changedSince(const OcclusionCacheEntry& entry, const osg::Vec3f& lightPos,
        const osg::Vec3f& objectPos, float objectRadius)
    {
        mOcclusionChanges.clear();
        if (!mOcclusionTest->getChangesSince(entry.mChangeCount, mOcclusionChanges))
            return true;
        // A door can change what the light reaches only if it moved across the way from the light to the object.
        const float margin = objectRadius + 16.f;
        for (osg::BoundingBox box : mOcclusionChanges)
        {
            box._min -= osg::Vec3f(margin, margin, margin);
            box._max += osg::Vec3f(margin, margin, margin);
            if (segmentHitsBox(lightPos, objectPos, box))
                return true;
        }
        return false;
    }

    void LightManager::getLightVisibility(const osg::RefMatrix* viewMatrix, size_t frameNum, double time,
        const osg::BoundingSphere& viewBound, const LightList& lightList, std::vector<float>& visibility,
        OcclusionCache& cache, const osg::BoundingBox* localBox, const osg::Matrix* modelView,
        const SurfaceSamples* localSamples)
    {
        visibility.assign(lightList.size(), 1.f);
        if (!mOcclusionEnabled || lightList.empty() || viewMatrix == nullptr || !viewBound.valid()
            || viewBound.radius() > mOcclusionMaxObjectRadius)
            return;

        if (mOcclusionFrame != frameNum)
        {
            mOcclusionFrame = frameNum;
            mOcclusionTimeUsed = 0.0;
            mOcclusionChangeCount = mOcclusionTest->getChangeCount();
        }
        if (mOcclusionInverseViewFor != viewMatrix || mOcclusionInverseViewFrame != frameNum)
        {
            mOcclusionInverseView = osg::Matrixd::inverse(*viewMatrix);
            mOcclusionInverseViewFor = viewMatrix;
            mOcclusionInverseViewFrame = frameNum;
        }

        // View space is world space rotated and moved (mirrored for reflections), so distances carry over.
        const osg::Vec3f objectPos = osg::Vec3d(viewBound.center()) * mOcclusionInverseView;
        const float radius = viewBound.radius();

        osg::BoundingBox worldBox;
        if (localBox != nullptr && modelView != nullptr && localBox->valid())
        {
            const osg::Matrixd toWorld = osg::Matrixd(*modelView) * mOcclusionInverseView;
            for (unsigned int i = 0; i < 8; ++i)
                worldBox.expandBy(osg::Vec3d(localBox->corner(i)) * toWorld);
        }
        const bool useSamples = localSamples != nullptr && !localSamples->empty() && modelView != nullptr;
        // made when first needed: most calls find every light in the cache
        mOcclusionWorldSamples.clear();
        const auto makeWorldSamples = [&] {
            if (!useSamples || !mOcclusionWorldSamples.empty())
                return;
            const osg::Matrixd toWorld = osg::Matrixd(*modelView) * mOcclusionInverseView;
            for (const SurfaceSample& sample : *localSamples)
            {
                osg::Vec3f normal = osg::Matrixd::transform3x3(osg::Vec3d(sample.mNormal), toWorld);
                normal.normalize();
                mOcclusionWorldSamples.push_back({ osg::Vec3d(sample.mPosition) * toWorld, normal });
            }
        };

        for (std::size_t i = 0; i < lightList.size(); ++i)
        {
            const LightSourceViewBound* light = lightList[i];
            const osg::Vec3f lightPos = osg::Vec3d(light->mViewBound.center()) * mOcclusionInverseView;
            // A light within the object (a lamp's own mesh) always reaches it. Not for objects tested at their
            // surfaces, which see to that themselves: a stair tunnel's bound takes in the room beside it, whose lamp
            // its walls hide.
            if (!useSamples && (lightPos - objectPos).length2() <= radius * radius)
                continue;

            OcclusionCacheEntry* entry = nullptr;
            for (OcclusionCacheEntry& candidate : cache)
                if (candidate.mLight == light->mLightSource)
                {
                    entry = &candidate;
                    break;
                }

            if (entry != nullptr)
                entry->mUsedFrame = frameNum;
            bool moved = entry == nullptr || (entry->mLightPos - lightPos).length2() > sOcclusionMoveTolerance2
                || (entry->mObjectPos - objectPos).length2() > sOcclusionMoveTolerance2;
            // A door turned across the way from the light: as urgent as a move.
            if (!moved && entry->mChangeCount != mOcclusionChangeCount)
            {
                if (changedSince(*entry, lightPos, objectPos, radius))
                    moved = true;
                else
                    entry->mChangeCount = mOcclusionChangeCount;
            }

            if (!moved && frameNum - entry->mFrame < sOcclusionRefreshFrames)
            {
                visibility[i] = fadedVisibility(*entry, time);
                continue;
            }

            // Out of time for this frame: keep what was found last, even if an end has moved (a carried lantern, a
            // walking NPC), until it can be tested again. Letting the light through meanwhile made things flick
            // between lit and dark as the player walked. A pair never tested yet gets the light. Pairs not tested
            // yet or moved come first: routine retests only get the time they leave over, else they used it all up
            // in a busy room and things coming into view stayed lit, then went dark seconds later.
            const double budget = moved ? mOcclusionNewBudget : mOcclusionRetestBudget;
            if (mOcclusionTimeUsed >= budget)
            {
                if (entry == nullptr)
                    ++mOcclusionStats.mWaiting;
                else
                {
                    ++mOcclusionStats.mPutOff;
                    visibility[i] = fadedVisibility(*entry, time);
                }
                continue;
            }

            const auto start = std::chrono::steady_clock::now();
            const unsigned int raysBefore = mOcclusionRaysUsed;
            makeWorldSamples();
            const float visible = useSamples ? testLightVisibilityFromSurface(lightPos, mOcclusionWorldSamples)
                : worldBox.valid() ? testLightVisibilityFromBox(lightPos, worldBox)
                                   : testLightVisibility(lightPos, objectPos, radius);
            const double spent
                = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
            mOcclusionTimeUsed += spent;
            mOcclusionStats.mTime += spent;
            ++mOcclusionStats.mTests;
            mOcclusionStats.mRays += mOcclusionRaysUsed - raysBefore;
            if (visible <= 0.f)
                ++mOcclusionStats.mBlocked;
            if (entry != nullptr && !moved && entry->mVisible != visible)
                ++mOcclusionStats.mFlips;

            if (entry == nullptr)
            {
                // Never tested: it was drawn with all of the light, so a hidden light fades out from there.
                cache.emplace_back();
                entry = &cache.back();
                entry->mLight = light->mLightSource;
                entry->mVisible = 1.f;
                entry->mFadeFrom = 1.f;
                entry->mFadeStart = time - sOcclusionFadeTime;
            }
            if (entry->mVisible != visible)
            {
                entry->mFadeFrom = fadedVisibility(*entry, time);
                entry->mFadeStart = time;
                entry->mVisible = visible;
            }
            entry->mLightPos = lightPos;
            entry->mObjectPos = objectPos;
            entry->mFrame = frameNum;
            entry->mUsedFrame = frameNum;
            entry->mChangeCount = mOcclusionChangeCount;
            visibility[i] = fadedVisibility(*entry, time);
        }

        // Results not needed for a long time (their light or object long out of view) are dropped.
        if (cache.size() > 32)
            std::erase_if(cache,
                [&](const OcclusionCacheEntry& entry) { return frameNum - entry.mUsedFrame > sOcclusionForgetFrames; });
    }

    void LightManager::removeOccludedLights(const osg::RefMatrix* viewMatrix, size_t frameNum, double time,
        const osg::BoundingSphere& viewBound, LightList& lightList, OcclusionCache& cache,
        const osg::BoundingBox* localBox, const osg::Matrix* modelView, const SurfaceSamples* localSamples)
    {
        std::vector<float> visibility;
        getLightVisibility(
            viewMatrix, frameNum, time, viewBound, lightList, visibility, cache, localBox, modelView, localSamples);
        // Per-object light lists take a light whole or not at all: kept while it reaches a fair part of the object.
        std::size_t kept = 0;
        for (std::size_t i = 0; i < lightList.size(); ++i)
            if (visibility[i] >= 0.25f)
                lightList[kept++] = lightList[i];
        lightList.resize(kept);
    }

    int LightManager::getMaxLights() const
    {
        return mMaxLights;
    }

    void LightManager::setMaxLights(int value)
    {
        mMaxLights = value;
    }

    Shader::ShaderManager::DefineMap LightManager::getLightDefines() const
    {
        Shader::ShaderManager::DefineMap defines;

        defines["maxLights"] = std::to_string(getMaxLights());
        defines["lightingMethodClustered"] = mClusteredLighting ? "1" : "0";
        defines["useGPUShader4"] = std::to_string(mClusteredLighting);
        defines["simpleLighting"] = "0";

        return defines;
    }

    void LightManager::processChangedSettings(
        float lightRadiusMultiplier, float maximumLightDistance, float lightFadeStart)
    {
        updateSettings(lightRadiusMultiplier, maximumLightDistance, lightFadeStart);
    }

    void LightManager::updateMaxLights(int maxLights)
    {
        setMaxLights(maxLights);

        getStateSet()->removeUniform("LightBuffer");
        getStateSet()->addUniform(generateLightBufferUniform());
    }

    void LightManager::updateSettings(float lightRadiusMultiplier, float maximumLightDistance, float lightFadeStart)
    {
        mPointLightRadiusMultiplier = lightRadiusMultiplier;
        mPointLightFadeEnd = maximumLightDistance;
        if (mPointLightFadeEnd > 0)
            mPointLightFadeStart = mPointLightFadeEnd * lightFadeStart;
        mCullCallback->reset();
    }

    void LightManager::enableClustered(bool enabled)
    {
        if (!enabled)
        {
            initPerObjectUniform(mMaxLights);
        }
        else
        {
            initClustered();
        }
    }

    void LightManager::initPerObjectUniform(int targetLights)
    {
        mClusteredLighting = false;
        setMaxLights(targetLights);

        getOrCreateStateSet()->addUniform(generateLightBufferUniform());

        mCullCallback->reset();
    }

    void LightManager::initClustered()
    {
        mClusteredLighting = true;
        mCullCallback->reset();
    }

    void LightManager::setLightingMask(size_t mask)
    {
        mLightingMask = mask;
    }

    size_t LightManager::getLightingMask() const
    {
        return mLightingMask;
    }

    void LightManager::update(size_t frameNum)
    {
        if (mPPLightBuffer)
            mPPLightBuffer->clear(frameNum);

        if (mOcclusionEnabled)
        {
            OcclusionStats& stats = mOcclusionStats;
            ++stats.mFrames;
            stats.mWorstFrameTime = std::max(stats.mWorstFrameTime, mOcclusionTimeUsed);
            mOcclusionTimeUsed = 0.0;
            if (stats.mFrames >= sOcclusionLogFrames)
            {
                if (stats.mTests > 0 || stats.mWaiting > 0)
                    Log(Debug::Info) << "Light occlusion, last " << stats.mFrames << " frames: " << stats.mTests
                                     << " tests (" << stats.mRays << " rays, " << stats.mBlocked << " hidden), "
                                     << stats.mTime / 1000.0 / static_cast<double>(stats.mFrames)
                                     << " ms a frame, worst frame " << stats.mWorstFrameTime / 1000.0
                                     << " ms; drawn lit while waiting for a first test " << stats.mWaiting
                                     << " times; retests put off " << stats.mPutOff
                                     << "; retests that changed with nothing moved " << stats.mFlips;
                stats = OcclusionStats{};
            }
        }

        mLights.clear();
        mLightsInViewSpace.clear();
        mLastViewSpaceCamera = nullptr;
        mLastViewSpaceLights = nullptr;
        mLightListStateSets.clear();
        mBlockedLightsStateSets.clear();
    }

    void LightManager::addLight(LightSource* lightSource, const osg::Matrixf& worldMat, size_t frameNum)
    {
        LightSourceTransform l;
        l.mLightSource = lightSource;
        l.mWorldMatrix = worldMat;
        osg::Vec3f pos = worldMat.getTrans();
        lightSource->getLight(frameNum)->setPosition(osg::Vec4f(pos, 1.f));

        mLights.push_back(l);
    }

    void LightManager::setSunlight(osg::ref_ptr<Light> sun)
    {
        mSun = sun;
    }

    osg::ref_ptr<Light> LightManager::getSunlight()
    {
        return mSun;
    }

    osg::ref_ptr<osg::StateSet> LightManager::getLightListStateSet(
        const LightList& lightList, size_t frameNum, const osg::RefMatrix* viewMatrix)
    {
        mLightListStateSetKey.first = viewMatrix;
        std::vector<int>& lightIds = mLightListStateSetKey.second;
        lightIds.clear();
        for (const LightSourceViewBound* light : lightList)
            lightIds.push_back(light->mLightSource->getId());

        auto found = mLightListStateSets.find(mLightListStateSetKey);
        if (found != mLightListStateSets.end())
            return found->second;
        found = mLightListStateSets.emplace(mLightListStateSetKey, nullptr).first;

        osg::ref_ptr<osg::StateSet> stateset = new osg::StateSet;
        osg::ref_ptr<osg::Uniform> data = generateLightBufferUniform();

        for (size_t i = 0; i < lightList.size(); ++i)
        {
            auto* light = lightList[i]->mLightSource->getLight(frameNum);
            osg::Matrixf lightMat;
            configurePosition(lightMat, light->getPosition() * (*viewMatrix));
            configureAmbient(lightMat, light->getAmbient());
            configureDiffuse(lightMat, light->getDiffuse());
            configureSpecular(lightMat, light->getSpecular());
            configureAttenuation(lightMat, light->getConstantAttenuation(), light->getLinearAttenuation(),
                light->getQuadraticAttenuation(),
                lightList[i]->mLightSource->getRadius() * mPointLightRadiusMultiplier);

            data->setElement(static_cast<unsigned int>(i), lightMat);
        }

        stateset->addUniform(data);
        stateset->addUniform(new osg::Uniform("PointLightCount", static_cast<int>(lightList.size())));

        found->second = stateset;
        return stateset;
    }

    osg::ref_ptr<osg::StateSet> LightManager::getBlockedLightsStateSet(const BlockedLightsMask& mask)
    {
        osg::ref_ptr<osg::StateSet>& stateset = mBlockedLightsStateSets[mask];
        if (!stateset)
        {
            stateset = new osg::StateSet;
            stateset->addUniform(makeBlockedLightsUniform(mask));
        }
        return stateset;
    }

    const std::vector<LightManager::LightSourceViewBound>& LightManager::getLightsInViewSpace(
        osgUtil::CullVisitor* cv, const osg::RefMatrix* viewMatrix, size_t frameNum)
    {
        return getViewSpaceLights(cv, viewMatrix, frameNum).mLights;
    }

    void LightManager::getLightsIntersecting(osgUtil::CullVisitor* cv, const osg::RefMatrix* viewMatrix,
        size_t frameNum, const osg::BoundingSphere& bound, const std::set<LightSource*>& ignored, LightList& out)
    {
        const ViewSpaceLights& lights = getViewSpaceLights(cv, viewMatrix, frameNum);
        // osg::BoundingSphere::intersects is false for an invalid sphere
        if (!bound.valid())
            return;

        // A light can only reach the bound when its centre is within both radii along every axis, so only lights whose
        // centre x is within reach are tested. The margin covers rounding in the sphere test, which is the one
        // osg::BoundingSphere::intersects does, term for term.
        const float x = bound.center().x();
        const float y = bound.center().y();
        const float z = bound.center().z();
        const float radius = bound.radius();
        const auto test = [&](std::size_t i) {
            const float dx = lights.mX[i] - x;
            const float dy = lights.mY[i] - y;
            const float dz = lights.mZ[i] - z;
            const float radii = lights.mRadius[i] + radius;
            if (!(dx * dx + dy * dy + dz * dz <= radii * radii))
                return;
            const LightSourceViewBound& light = lights.mLights[lights.mIndex[i]];
            if (!ignored.empty() && ignored.contains(light.mLightSource))
                return;
            out.push_back(&light);
        };

        const std::size_t numSearched = lights.mX.size() - lights.mNumWide;
        const float reach = (radius + lights.mMaxRadius) * 1.0001f + 1.f;
        const auto searchedEnd = lights.mX.begin() + static_cast<std::ptrdiff_t>(numSearched);
        const std::size_t begin
            = static_cast<std::size_t>(std::lower_bound(lights.mX.begin(), searchedEnd, x - reach) - lights.mX.begin());
        const float last = x + reach;
        for (std::size_t i = begin; i < numSearched && lights.mX[i] <= last; ++i)
            test(i);
        for (std::size_t i = numSearched; i < lights.mX.size(); ++i)
            test(i);
    }

    LightManager::ViewSpaceLights& LightManager::getViewSpaceLights(
        osgUtil::CullVisitor* cv, const osg::RefMatrix* viewMatrix, size_t frameNum)
    {
        osg::Camera* camera = cv->getCurrentCamera();

        if (mLastViewSpaceLights != nullptr && camera == mLastViewSpaceCamera && frameNum == mLastViewSpaceFrame)
            return *mLastViewSpaceLights;

        osg::observer_ptr<osg::Camera> camPtr(camera);
        auto it = mLightsInViewSpace.find(camPtr);

        if (it == mLightsInViewSpace.end())
        {
            it = mLightsInViewSpace.insert(std::make_pair(camPtr, ViewSpaceLights())).first;
            LightSourceViewBoundCollection& collection = it->second.mLights;

            for (const auto& transform : mLights)
            {
                osg::Matrixf worldViewMat = transform.mWorldMatrix * (*viewMatrix);

                float radius = transform.mLightSource->getRadius() * mPointLightRadiusMultiplier;

                osg::BoundingSphere viewBound(osg::Vec3f(), radius);
                transformBoundingSphere(worldViewMat, viewBound);

                if (transform.mLightSource->getLastAppliedFrame() != frameNum && mPointLightFadeEnd != 0.f)
                {
                    const float fadeDelta = mPointLightFadeEnd - mPointLightFadeStart;
                    const float viewDelta = viewBound.center().length() - mPointLightFadeStart;
                    float fade = 1 - std::clamp(viewDelta / fadeDelta, 0.f, 1.f);
                    if (fade == 0.f)
                        continue;

                    auto* light = transform.mLightSource->getLight(frameNum);
                    light->setDiffuse(light->getDiffuse() * fade);
                    light->setSpecular(light->getSpecular() * fade);
                    transform.mLightSource->setLastAppliedFrame(frameNum);
                }

                LightSourceViewBound l;
                l.mLightSource = transform.mLightSource;
                l.mViewBound = viewBound;
                collection.push_back(l);
            }

            const bool fillPPBuffer = mPPLightBuffer && it->first->getName() == Constants::SceneCamera;

            if (mClusteredLighting || fillPPBuffer)
            {
                auto sorter = [](const LightManager::LightSourceViewBound& left,
                                  const LightManager::LightSourceViewBound& right) {
                    return left.mViewBound.center().length2() - left.mViewBound.radius2()
                        < right.mViewBound.center().length2() - right.mViewBound.radius2();
                };

                std::sort(collection.begin(), collection.end(), sorter);

                osg::CullingSet& cullingSet = cv->getModelViewCullingStack().front();
                for (auto& bound : collection)
                {
                    const auto* light = bound.mLightSource->getLight(frameNum);
                    const float radius = bound.mLightSource->getRadius() * mPointLightRadiusMultiplier;
                    osg::BoundingSphere frustumBound = bound.mViewBound;
                    frustumBound.radius() = radius * 2.f;

                    bound.mCulled = cullingSet.isCulled(frustumBound);

                    if (bound.mCulled || bound.mLightSource->getEmpty() || light->getDiffuse().x() < 0.f)
                        continue;
                }

                if (fillPPBuffer)
                    fillPPLights(collection, frameNum, cv->getFrameStamp()->getReferenceTime());
            }

            // Lights reaching more than twice as far as the typical one (the median) are tested for every object
            // instead of being searched along x, where they would widen the search for all the others.
            ViewSpaceLights& lights = it->second;
            const std::size_t count = collection.size();
            float wideRadius = 0.f;
            if (count > 0)
            {
                std::vector<float> radii;
                radii.reserve(count);
                for (const LightSourceViewBound& light : collection)
                    radii.push_back(light.mViewBound.radius());
                std::nth_element(
                    radii.begin(), radii.begin() + static_cast<std::ptrdiff_t>(count / 2), radii.end());
                wideRadius = 2.f * radii[count / 2];
            }

            std::vector<std::uint32_t> order;
            order.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
                if (collection[i].mViewBound.radius() <= wideRadius)
                    order.push_back(i);
            std::sort(order.begin(), order.end(), [&](std::uint32_t left, std::uint32_t right) {
                return collection[left].mViewBound.center().x() < collection[right].mViewBound.center().x();
            });
            const std::size_t numSearched = order.size();
            for (std::uint32_t i = 0; i < count; ++i)
                if (collection[i].mViewBound.radius() > wideRadius)
                    order.push_back(i);
            lights.mNumWide = order.size() - numSearched;

            lights.mX.reserve(count);
            lights.mY.reserve(count);
            lights.mZ.reserve(count);
            lights.mRadius.reserve(count);
            for (std::size_t i = 0; i < order.size(); ++i)
            {
                const osg::BoundingSphere& bound = collection[order[i]].mViewBound;
                lights.mX.push_back(bound.center().x());
                lights.mY.push_back(bound.center().y());
                lights.mZ.push_back(bound.center().z());
                lights.mRadius.push_back(bound.radius());
                if (i < numSearched)
                    lights.mMaxRadius = std::max(lights.mMaxRadius, bound.radius());
            }
            lights.mIndex = std::move(order);
        }

        mLastViewSpaceCamera = camera;
        mLastViewSpaceFrame = frameNum;
        mLastViewSpaceLights = &it->second;
        return it->second;
    }

    void LightManager::fillPPLights(const LightSourceViewBoundCollection& collection, size_t frameNum, double time)
    {
        // A light fades fully in or out over this long.
        constexpr float fadeSeconds = 0.3f;
        // Slots kept for lights fading out, so a light leaving the set still has room to fade.
        constexpr std::size_t fadingOutSlots = 8;
        constexpr std::size_t inSetSlots = PPLightBuffer::sMaxPPLights - fadingOutSlots;

        // After a pause or a loading screen the set is taken as it is.
        const double elapsed = mPPLastTime < 0.0 ? 1.0 : std::clamp(time - mPPLastTime, 0.0, 1.0);
        mPPLastTime = time;
        const float step = static_cast<float>(elapsed) / fadeSeconds;

        const auto usable = [&](const LightSourceViewBound& bound) {
            return !bound.mCulled && !bound.mLightSource->getEmpty()
                && bound.mLightSource->getLight(frameNum)->getDiffuse().x() >= 0.f;
        };

        // The nearest usable lights (collection is sorted nearest first) make the set and fade in; every other light
        // fades out. A light outside the view keeps its weight: it can't be seen, so it neither fades in nor out.
        std::size_t inSet = 0;
        std::vector<std::pair<const LightSourceViewBound*, float>> fadingOut;
        const std::shared_ptr<PPLightBuffer>& buffer = getPPLightsBuffer();
        for (const LightSourceViewBound& bound : collection)
        {
            const int id = bound.mLightSource->getId();
            if (!usable(bound))
            {
                if (const auto fade = mPPLightFades.find(id); fade != mPPLightFades.end())
                    fade->second.mFrame = frameNum;
                continue;
            }

            PPLightFade& fade = mPPLightFades[id];
            fade.mFrame = frameNum;
            const float radius = bound.mLightSource->getRadius() * mPointLightRadiusMultiplier;
            if (inSet < inSetSlots)
            {
                ++inSet;
                fade.mWeight = std::min(1.f, fade.mWeight + step);
                buffer->setLight(frameNum, bound.mLightSource->getLight(frameNum), radius, fade.mWeight);
            }
            else
            {
                fade.mWeight = std::max(0.f, fade.mWeight - step);
                if (fade.mWeight > 0.f)
                    fadingOut.emplace_back(&bound, fade.mWeight);
            }
        }

        // the brightest of the lights fading out get the spare slots
        std::sort(fadingOut.begin(), fadingOut.end(),
            [](const auto& left, const auto& right) { return left.second > right.second; });
        if (fadingOut.size() > fadingOutSlots)
            fadingOut.resize(fadingOutSlots);
        for (const auto& [bound, weight] : fadingOut)
            buffer->setLight(frameNum, bound->mLightSource->getLight(frameNum),
                bound->mLightSource->getRadius() * mPointLightRadiusMultiplier, weight);

        // forget lights gone from the scene, and ones fully faded out
        std::erase_if(mPPLightFades, [&](const auto& entry) {
            return entry.second.mFrame != frameNum || entry.second.mWeight <= 0.f;
        });
    }

    osg::ref_ptr<osg::Uniform> LightManager::generateLightBufferUniform()
    {
        osg::ref_ptr<osg::Uniform> uniform = new osg::Uniform(osg::Uniform::FLOAT_MAT4, "LightBuffer", getMaxLights());

        return uniform;
    }

    void LightManager::setCollectPPLights(bool enabled)
    {
        if (enabled)
            mPPLightBuffer = std::make_shared<PPLightBuffer>();
        else
            mPPLightBuffer = nullptr;
    }

    LightSource::LightSource()
        : mRadius(0.f)
        , mActorFade(1.f)
        , mLastAppliedFrame(0)
    {
        setUpdateCallback(new CollectLightCallback);
        mId = sLightId++;
    }

    LightSource::LightSource(const LightSource& copy, const osg::CopyOp& copyop)
        : osg::Node(copy, copyop)
        , mRadius(copy.mRadius)
        , mActorFade(copy.mActorFade)
        , mBounce(copy.mBounce)
        , mLastAppliedFrame(copy.mLastAppliedFrame)
    {
        mId = sLightId++;

        for (size_t i = 0; i < mLight.size(); ++i)
            mLight[i] = new Light(*copy.mLight[i].get(), copyop);
    }

    void LightListCallback::operator()(osg::Node* node, osgUtil::CullVisitor* cv)
    {
        if (!mLightManager)
            mLightManager = findLightManager(cv->getNodePath());
        if (mLightManager && mLightManager->getClusteredLighting())
        {
            const int pushed = pushBlockedLightsState(node, cv);
            if (pushed == 2)
                ++mLightManager->mBlockedMaskDepth;
            traverse(node, cv);
            if (pushed == 2)
                --mLightManager->mBlockedMaskDepth;
            if (pushed != 0)
                cv->popStateSet();
            return;
        }

        bool pushedState = pushLightState(node, cv);
        traverse(node, cv);
        if (pushedState)
            cv->popStateSet();
    }

    namespace
    {
        struct SurfaceTriangle
        {
            osg::Vec3f mA, mB, mC;
            osg::Vec3f mNormal;
            float mArea;
        };

        struct SurfaceTriangleCollector
        {
            const osg::Vec3Array* mVertices = nullptr;
            const osg::Vec3Array* mNormals = nullptr;
            osg::Matrixd mMatrix;
            std::vector<SurfaceTriangle>* mOut = nullptr;

            void operator()(unsigned int i1, unsigned int i2, unsigned int i3)
            {
                if (i1 >= mVertices->size() || i2 >= mVertices->size() || i3 >= mVertices->size())
                    return;
                const osg::Vec3f a = osg::Vec3d((*mVertices)[i1]) * mMatrix;
                const osg::Vec3f b = osg::Vec3d((*mVertices)[i2]) * mMatrix;
                const osg::Vec3f c = osg::Vec3d((*mVertices)[i3]) * mMatrix;
                osg::Vec3f normal = (b - a) ^ (c - a);
                const float area = 0.5f * normal.normalize();
                if (!(area > 1.f))
                    return;
                // the side it is drawn on, by its corners' normals where it has them
                if (mNormals != nullptr)
                {
                    osg::Vec3f average;
                    for (unsigned int i : { i1, i2, i3 })
                        average += osg::Matrixd::transform3x3(osg::Vec3d((*mNormals)[i]), mMatrix);
                    if (average * normal < 0.f)
                        normal = -normal;
                }
                mOut->push_back({ a, b, c, normal, area });
            }
        };

        // The triangles of a node's meshes in its own space; marks it animated if a skinned or morphing mesh is among
        // them (an actor), whose triangles move.
        class SurfaceSampleVisitor : public osg::NodeVisitor
        {
        public:
            SurfaceSampleVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
                mMatrices.emplace_back();
            }

            void apply(osg::Transform& transform) override
            {
                osg::Matrix matrix = mMatrices.back();
                transform.computeLocalToWorldMatrix(matrix, this);
                mMatrices.push_back(matrix);
                traverse(transform);
                mMatrices.pop_back();
            }

            void apply(osg::Drawable& drawable) override
            {
                if (dynamic_cast<RigGeometry*>(&drawable) || dynamic_cast<MorphGeometry*>(&drawable)
                    || dynamic_cast<RigGeometryHolder*>(&drawable))
                {
                    mAnimated = true;
                    return;
                }
                osg::Geometry* geometry = drawable.asGeometry();
                if (geometry == nullptr)
                    return;
                const auto* vertices = dynamic_cast<const osg::Vec3Array*>(geometry->getVertexArray());
                if (vertices == nullptr || vertices->empty())
                    return;
                const auto* normals = dynamic_cast<const osg::Vec3Array*>(geometry->getNormalArray());
                if (normals != nullptr
                    && (geometry->getNormalBinding() != osg::Geometry::BIND_PER_VERTEX
                        || normals->size() != vertices->size()))
                    normals = nullptr;
                osg::TriangleIndexFunctor<SurfaceTriangleCollector> functor;
                functor.mVertices = vertices;
                functor.mNormals = normals;
                functor.mMatrix = mMatrices.back();
                functor.mOut = &mTriangles;
                geometry->accept(functor);
            }

            std::vector<SurfaceTriangle> mTriangles;
            bool mAnimated = false;

        private:
            std::vector<osg::Matrix> mMatrices;
        };

        // 16 points spread over the surfaces: 128 picked across them by area, then the 16 furthest from each other.
        LightManager::SurfaceSamples sampleSurfaces(const std::vector<SurfaceTriangle>& triangles)
        {
            LightManager::SurfaceSamples result;
            if (triangles.empty())
                return result;
            std::vector<double> cumulative;
            cumulative.reserve(triangles.size());
            double total = 0.0;
            for (const SurfaceTriangle& triangle : triangles)
                cumulative.push_back(total += triangle.mArea);

            constexpr unsigned int candidateCount = 128;
            constexpr unsigned int sampleCount = 16;
            static constexpr float weights[4][3]
                = { { 1 / 3.f, 1 / 3.f, 1 / 3.f }, { 0.6f, 0.2f, 0.2f }, { 0.2f, 0.6f, 0.2f }, { 0.2f, 0.2f, 0.6f } };
            LightManager::SurfaceSamples candidates;
            candidates.reserve(candidateCount);
            osg::Vec3f centre;
            for (unsigned int i = 0; i < candidateCount; ++i)
            {
                const double at = (i + 0.5) / candidateCount * total;
                const std::size_t index = std::min<std::size_t>(
                    std::upper_bound(cumulative.begin(), cumulative.end(), at) - cumulative.begin(),
                    triangles.size() - 1);
                const SurfaceTriangle& triangle = triangles[index];
                const float* w = weights[i % 4];
                const osg::Vec3f position = triangle.mA * w[0] + triangle.mB * w[1] + triangle.mC * w[2];
                candidates.push_back({ position, triangle.mNormal });
                centre += position;
            }
            centre /= static_cast<float>(candidates.size());

            std::vector<float> nearest(candidates.size(), std::numeric_limits<float>::max());
            std::size_t next = 0;
            float furthest = -1.f;
            for (std::size_t i = 0; i < candidates.size(); ++i)
            {
                const float d = (candidates[i].mPosition - centre).length2();
                if (d > furthest)
                {
                    furthest = d;
                    next = i;
                }
            }
            while (result.size() < sampleCount && result.size() < candidates.size())
            {
                result.push_back(candidates[next]);
                const osg::Vec3f picked = candidates[next].mPosition;
                furthest = -1.f;
                for (std::size_t i = 0; i < candidates.size(); ++i)
                {
                    nearest[i] = std::min(nearest[i], (candidates[i].mPosition - picked).length2());
                    if (nearest[i] > furthest)
                    {
                        furthest = nearest[i];
                        next = i;
                    }
                }
                if (furthest <= 0.f)
                    break;
            }
            return result;
        }
    }

    const osg::BoundingBox* LightListCallback::getOcclusionBox(osg::Node* node)
    {
        // The bound in the same space as the box (see pushLightState).
        osg::BoundingSphere bound;
        const osg::Transform* transform = node->asTransform();
        if (transform)
        {
            for (unsigned int i = 0; i < transform->getNumChildren(); ++i)
                bound.expandBy(transform->getChild(i)->getBound());
        }
        else
            bound = node->getBound();

        // Only tiny things are tested around their bounding sphere. For anything bigger the box matters: walls,
        // floors, bay windows and hanging signs have their centres in or beside the wall they are on, far from the
        // side a light shines on.
        constexpr float minRadius = 24.f;
        if (!bound.valid() || bound.radius() < minRadius)
            return nullptr;

        // In the node's own space, so it changes only as an animation moves the parts (a walking NPC's bound
        // shifts a little every frame): made again only once it has changed by a quarter of its size.
        const float tolerance = 0.25f * bound.radius();
        if (!mHasOcclusionBox || (bound.center() - mOcclusionBoxFor.center()).length() > tolerance
            || std::abs(bound.radius() - mOcclusionBoxFor.radius()) > tolerance)
        {
            osg::ComputeBoundsVisitor visitor;
            if (transform)
            {
                for (unsigned int i = 0; i < transform->getNumChildren(); ++i)
                    const_cast<osg::Node*>(transform->getChild(i))->accept(visitor);
            }
            else
                node->accept(visitor);
            mOcclusionBox = visitor.getBoundingBox();
            mOcclusionBoxFor = bound;
            mHasOcclusionBox = true;

            // Not for objects too big for light occlusion (merged chunks of statics), whose triangles are many.
            mOcclusionSamples.clear();
            if (bound.radius() <= mLightManager->getOcclusionMaxObjectRadius())
            {
                SurfaceSampleVisitor samples;
                if (transform)
                {
                    for (unsigned int i = 0; i < transform->getNumChildren(); ++i)
                        const_cast<osg::Node*>(transform->getChild(i))->accept(samples);
                }
                else
                    node->accept(samples);
                if (!samples.mAnimated)
                    mOcclusionSamples = sampleSurfaces(samples.mTriangles);
            }
        }
        return mOcclusionBox.valid() ? &mOcclusionBox : nullptr;
    }

    int LightListCallback::pushBlockedLightsState(osg::Node* node, osgUtil::CullVisitor* cv)
    {
        CullProfile::Scope profile(CullProfile::Section::LightLists);

        if (!mLightManager->getLightOcclusionEnabled())
            return 0;
        if (!(cv->getTraversalMask() & mLightManager->getLightingMask()))
            return 0;

        const osg::Camera* camera = cv->getCurrentCamera();
        const size_t frameNum = cv->getTraversalNumber();
        if (camera != mBlockedCamera || frameNum != mBlockedFrame)
        {
            mBlockedCamera = camera;
            mBlockedFrame = frameNum;
            mBlockedStateSet = nullptr;
            mBlockedAny = false;

            const osg::RefMatrix* viewMatrix = cv->getCurrentRenderStage()->getInitialViewMatrix();

            osg::BoundingSphere nodeBound;
            const osg::Transform* transform = node->asTransform();
            if (transform)
            {
                for (unsigned int i = 0; i < transform->getNumChildren(); ++i)
                    nodeBound.expandBy(transform->getChild(i)->getBound());
            }
            else
                nodeBound = node->getBound();
            transformBoundingSphere(*cv->getModelViewMatrix(), nodeBound);

            mLightList.clear();
            mLightManager->getLightsIntersecting(cv, viewMatrix, frameNum, nodeBound, mIgnoredLightSources, mLightList);
            if (!mLightList.empty())
            {
                const osg::BoundingBox* box = getOcclusionBox(node);
                mLightManager->getLightVisibility(viewMatrix, frameNum, cv->getFrameStamp()->getReferenceTime(),
                    nodeBound, mLightList, mLightVisibility, mOcclusionCache, box, cv->getModelViewMatrix(),
                    getOcclusionSamples());
                LightManager::BlockedLightsMask mask{};
                for (std::size_t i = 0; i < mLightList.size(); ++i)
                {
                    const int index = mLightList[i]->mGpuIndex;
                    if (index < 0 || index >= static_cast<int>(LightManager::sMaxBlockableLights))
                        continue;
                    const unsigned int shade = static_cast<unsigned int>(std::lround(
                        (1.f - std::clamp(mLightVisibility[i], 0.f, 1.f)) * LightManager::sLightShadeLevels));
                    if (shade == 0)
                        continue;
                    const auto at = static_cast<unsigned int>(index);
                    mask[at / 8] |= shade << (4 * (at % 8));
                    mBlockedAny = true;
                }
                if (mBlockedAny)
                    mBlockedStateSet = mLightManager->getBlockedLightsStateSet(mask);
            }
        }

        if (mBlockedAny)
        {
            cv->pushStateSet(mBlockedStateSet);
            return 2;
        }
        // Inside an object whose mask hides lights from it, this one needs its own (empty) mask.
        if (mLightManager->mBlockedMaskDepth > 0)
        {
            cv->pushStateSet(mLightManager->getBlockedLightsStateSet({}));
            return 1;
        }
        return 0;
    }

    bool LightListCallback::pushLightState(osg::Node* node, osgUtil::CullVisitor* cv)
    {
        CullProfile::Scope profile(CullProfile::Section::LightLists);

        if (!mLightManager)
        {
            mLightManager = findLightManager(cv->getNodePath());
            if (!mLightManager)
                return false;
        }

        // This cull callback SHOULD REALLY NOT EXIST when clustered shading is active!
        if (mLightManager->getClusteredLighting())
            return false;

        if (!(cv->getTraversalMask() & mLightManager->getLightingMask()))
            return false;

        // Possible optimizations:
        // - organize lights in a quad tree

        // Don't use Camera::getViewMatrix, that one might be relative to another camera!
        const osg::RefMatrix* viewMatrix = cv->getCurrentRenderStage()->getInitialViewMatrix();

        // Update light list if necessary
        // This makes sure we don't update it more than once per frame when rendering with multiple cameras
        if (mLastFrameNumber != cv->getTraversalNumber())
        {
            mLastFrameNumber = cv->getTraversalNumber();

            // Get the node bounds in view space
            // NB: do not node->getBound() * modelView, that would apply the node's transformation twice
            osg::BoundingSphere nodeBound;
            const osg::Transform* transform = node->asTransform();
            if (transform)
            {
                for (unsigned int i = 0; i < transform->getNumChildren(); ++i)
                    nodeBound.expandBy(transform->getChild(i)->getBound());
            }
            else
                nodeBound = node->getBound();

            transformBoundingSphere(*cv->getModelViewMatrix(), nodeBound);

            mLightList.clear();
            mLightManager->getLightsIntersecting(
                cv, viewMatrix, mLastFrameNumber, nodeBound, mIgnoredLightSources, mLightList);
            if (mLightManager->getLightOcclusionEnabled())
            {
                const osg::BoundingBox* box = getOcclusionBox(node);
                mLightManager->removeOccludedLights(viewMatrix, mLastFrameNumber,
                    cv->getFrameStamp()->getReferenceTime(), nodeBound, mLightList, mOcclusionCache, box,
                    cv->getModelViewMatrix(), getOcclusionSamples());
            }

            const size_t maxLights = mLightManager->getMaxLights();

            if (mLightList.size() > maxLights)
            {
                // Sort by proximity to object: prefer closer lights with larger radius
                std::sort(mLightList.begin(), mLightList.end(),
                    [&](const SceneUtil::LightManager::LightSourceViewBound* left,
                        const SceneUtil::LightManager::LightSourceViewBound* right) {
                        const float leftDist = (nodeBound.center() - left->mViewBound.center()).length2();
                        const float rightDist = (nodeBound.center() - right->mViewBound.center()).length2();
                        // A tricky way to compare normalized distance. This avoids division by near zero
                        return left->mViewBound.radius() * rightDist > right->mViewBound.radius() * leftDist;
                    });

                mLightList.resize(maxLights);
            }
        }

        if (!mLightList.empty())
        {
            cv->pushStateSet(mLightManager->getLightListStateSet(mLightList, mLastFrameNumber, viewMatrix));
            return true;
        }
        return false;
    }
}
