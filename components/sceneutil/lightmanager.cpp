#include "lightmanager.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <osg/ComputeBoundsVisitor>
#include <osgUtil/CullVisitor>

#include <components/debug/debuglog.hpp>
#include <components/misc/constants.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/util.hpp>
#include <components/shader/shadermanager.hpp>

#include "cullprofile.hpp"
#include "memorybarrier.hpp"

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
        getOrCreateStateSet()->addUniform(new osg::Uniform("blockedLights", 0u, 0u, 0u, 0u));
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

    void LightManager::setLightOcclusion(
        LightOcclusionTest* test, bool enabled, unsigned int raysPerFrame, float maxObjectRadius)
    {
        mOcclusionTest = test;
        mOcclusionEnabled = enabled && test != nullptr;
        mOcclusionRaysPerFrame = raysPerFrame;
        mOcclusionMaxObjectRadius = maxObjectRadius;
    }

    namespace
    {
        // A cached result stands while neither end has moved further than this (world units)...
        constexpr float sOcclusionMoveTolerance2 = 24.f * 24.f;
        // ...and is retested after this many frames anyway (doors open and close).
        constexpr size_t sOcclusionRefreshFrames = 90;
        // Results not used for this long are dropped.
        constexpr size_t sOcclusionForgetFrames = 600;
    }

    bool LightManager::isLightHidden(const osg::Vec3f& lightPos, const osg::Vec3f& objectPos, float objectRadius)
    {
        // Hidden only when no part of the object can be reached: its centre and six points halfway out. The centre
        // goes first, and most lights see it.
        const float r = objectRadius * 0.5f;
        const osg::Vec3f offsets[]
            = { { 0, 0, 0 }, { 0, 0, r }, { r, 0, 0 }, { -r, 0, 0 }, { 0, r, 0 }, { 0, -r, 0 }, { 0, 0, -r } };
        for (const osg::Vec3f& offset : offsets)
        {
            ++mOcclusionRaysUsed;
            if (!mOcclusionTest->isBlocked(lightPos, objectPos + offset))
                return false;
        }
        return true;
    }

    bool LightManager::isLightHiddenFromBox(const osg::Vec3f& lightPos, const osg::BoundingBox& box)
    {
        // A light inside the object's box (a lamp in a room part, a candle on a table) always reaches it.
        if (box.contains(lightPos))
            return false;

        // Points just inside the box, so the ray ends in the object itself (whose own collision shape the test leaves
        // out) rather than in front of it.
        constexpr float inset = 16.f;
        const osg::Vec3f center = box.center();
        const auto inside = [&](osg::Vec3f point) {
            for (int axis = 0; axis < 3; ++axis)
            {
                if (point[axis] > center[axis])
                    point[axis] = std::max(center[axis], point[axis] - inset);
                else
                    point[axis] = std::min(center[axis], point[axis] + inset);
            }
            return point;
        };
        const auto visible = [&](const osg::Vec3f& point) {
            ++mOcclusionRaysUsed;
            return !mOcclusionTest->isBlocked(lightPos, point);
        };

        // The part of the object nearest the light goes first: a wall or floor beside a lamp is lit there.
        osg::Vec3f nearest;
        for (int axis = 0; axis < 3; ++axis)
            nearest[axis] = std::clamp(lightPos[axis], box._min[axis], box._max[axis]);
        if (visible(inside(nearest)))
            return false;
        // then the middle of each side facing the light, and the centre
        for (int axis = 0; axis < 3; ++axis)
        {
            if (lightPos[axis] >= box._min[axis] && lightPos[axis] <= box._max[axis])
                continue;
            osg::Vec3f faceCenter = center;
            faceCenter[axis] = lightPos[axis] < box._min[axis] ? box._min[axis] : box._max[axis];
            if (visible(inside(faceCenter)))
                return false;
        }
        return !visible(center);
    }

    void LightManager::removeOccludedLights(const osg::RefMatrix* viewMatrix, size_t frameNum,
        const osg::BoundingSphere& viewBound, LightList& lightList, OcclusionCache& cache,
        const osg::BoundingBox* localBox, const osg::Matrix* modelView)
    {
        if (!mOcclusionEnabled || lightList.empty() || viewMatrix == nullptr || !viewBound.valid()
            || viewBound.radius() > mOcclusionMaxObjectRadius)
            return;

        if (mOcclusionFrame != frameNum)
        {
            mOcclusionFrame = frameNum;
            mOcclusionRaysUsed = 0;
        }
        if (mOcclusionInverseViewFor != viewMatrix || mOcclusionInverseViewFrame != frameNum)
        {
            mOcclusionInverseView = osg::Matrixf::inverse(*viewMatrix);
            mOcclusionInverseViewFor = viewMatrix;
            mOcclusionInverseViewFrame = frameNum;
        }

        // View space is world space rotated and moved (mirrored for reflections), so distances carry over.
        const osg::Vec3f objectPos = viewBound.center() * mOcclusionInverseView;
        const float radius = viewBound.radius();

        osg::BoundingBox worldBox;
        if (localBox != nullptr && modelView != nullptr && localBox->valid())
        {
            const osg::Matrix toWorld = *modelView * osg::Matrix(mOcclusionInverseView);
            for (unsigned int i = 0; i < 8; ++i)
                worldBox.expandBy(localBox->corner(i) * toWorld);
        }

        const auto isHidden = [&](const LightSourceViewBound* light) {
            const osg::Vec3f lightPos = light->mViewBound.center() * mOcclusionInverseView;
            // A light within the object (a lamp's own mesh, a room around it) always reaches it.
            if ((lightPos - objectPos).length2() <= radius * radius)
                return false;

            OcclusionCacheEntry* entry = nullptr;
            for (OcclusionCacheEntry& candidate : cache)
                if (candidate.mLight == light->mLightSource)
                {
                    entry = &candidate;
                    break;
                }

            const bool moved = entry == nullptr || (entry->mLightPos - lightPos).length2() > sOcclusionMoveTolerance2
                || (entry->mObjectPos - objectPos).length2() > sOcclusionMoveTolerance2;
            if (!moved && frameNum - entry->mFrame < sOcclusionRefreshFrames)
                return entry->mBlocked;

            // Out of tests for this frame: keep what was found last, even if an end has moved (a carried lantern, a
            // walking NPC), until it can be tested again. Letting the light through meanwhile made things flick
            // between lit and dark as the player walked, whenever the tests ran out. A pair never tested yet gets
            // the light.
            if (mOcclusionRaysUsed >= mOcclusionRaysPerFrame)
                return entry != nullptr && entry->mBlocked;

            const bool blocked = worldBox.valid() ? isLightHiddenFromBox(lightPos, worldBox)
                                                  : isLightHidden(lightPos, objectPos, radius);
            if (entry == nullptr)
            {
                cache.emplace_back();
                entry = &cache.back();
                entry->mLight = light->mLightSource;
            }
            entry->mLightPos = lightPos;
            entry->mObjectPos = objectPos;
            entry->mFrame = frameNum;
            entry->mBlocked = blocked;
            return blocked;
        };

        lightList.erase(std::remove_if(lightList.begin(), lightList.end(), isHidden), lightList.end());

        if (cache.size() > 32)
            std::erase_if(cache,
                [&](const OcclusionCacheEntry& entry) { return frameNum - entry.mFrame > sOcclusionForgetFrames; });
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

    osg::ref_ptr<osg::StateSet> LightManager::getBlockedLightsStateSet(const std::array<unsigned int, 4>& mask)
    {
        osg::ref_ptr<osg::StateSet>& stateset = mBlockedLightsStateSets[mask];
        if (!stateset)
        {
            stateset = new osg::StateSet;
            stateset->addUniform(new osg::Uniform("blockedLights", mask[0], mask[1], mask[2], mask[3]));
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
                LightManager::LightList reaching = mLightList;
                mLightManager->removeOccludedLights(viewMatrix, frameNum, nodeBound, reaching, mOcclusionCache,
                    getOcclusionBox(node), cv->getModelViewMatrix());
                if (reaching.size() != mLightList.size())
                {
                    std::array<unsigned int, 4> mask{};
                    for (const LightManager::LightSourceViewBound* light : mLightList)
                    {
                        const int index = light->mGpuIndex;
                        if (index < 0 || index >= 128
                            || std::find(reaching.begin(), reaching.end(), light) != reaching.end())
                            continue;
                        mask[static_cast<std::size_t>(index) / 32] |= 1u << (static_cast<unsigned int>(index) % 32);
                        mBlockedAny = true;
                    }
                    if (mBlockedAny)
                        mBlockedStateSet = mLightManager->getBlockedLightsStateSet(mask);
                }
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
                mLightManager->removeOccludedLights(viewMatrix, mLastFrameNumber, nodeBound, mLightList,
                    mOcclusionCache, getOcclusionBox(node), cv->getModelViewMatrix());

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
