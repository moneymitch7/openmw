#ifndef OPENMW_COMPONENTS_SCENEUTIL_LIGHTMANAGER_H
#define OPENMW_COMPONENTS_SCENEUTIL_LIGHTMANAGER_H

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

#include <osg/BoundingBox>
#include <osg/BufferIndexBinding>
#include <osg/BufferTemplate>
#include <osg/DispatchCompute>
#include <osg/Group>
#include <osg/NodeVisitor>
#include <osg/observer_ptr>

#include <components/misc/constants.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/sceneutil/clusteredlighting.hpp>
#include <components/sceneutil/nodecallback.hpp>

namespace SceneUtil
{
    template <class T>
    using DoubleBuffer = std::array<T, 2>;

    class Light : public osg::Referenced
    {
    public:
        Light() = default;

        Light(const Light& copy, const osg::CopyOp& copyop)
            : mSpecular(copy.mSpecular)
            , mDiffuse(copy.mDiffuse)
            , mAmbient(copy.mAmbient)
            , mPosition(copy.mPosition)
            , mConstantAttenuation(copy.mConstantAttenuation)
            , mLinearAttenuation(copy.mLinearAttenuation)
            , mQuadraticAttenuation(copy.mQuadraticAttenuation)
        {
        }

        const osg::Vec4f& getSpecular() const { return mSpecular; }
        const osg::Vec4f& getDiffuse() const { return mDiffuse; }
        const osg::Vec4f& getAmbient() const { return mAmbient; }
        const osg::Vec4f& getPosition() const { return mPosition; }
        float getLinearAttenuation() const { return mLinearAttenuation; }
        float getConstantAttenuation() const { return mConstantAttenuation; }
        float getQuadraticAttenuation() const { return mQuadraticAttenuation; }

        void setSpecular(const osg::Vec4f& specular) { mSpecular = specular; }
        void setDiffuse(const osg::Vec4f& diffuse) { mDiffuse = diffuse; }
        void setAmbient(const osg::Vec4f& ambient) { mAmbient = ambient; }
        void setPosition(const osg::Vec4f& position) { mPosition = position; }
        void setLinearAttenuation(float att) { mLinearAttenuation = att; }
        void setConstantAttenuation(float att) { mConstantAttenuation = att; }
        void setQuadraticAttenuation(float att) { mQuadraticAttenuation = att; }

    private:
        osg::Vec4f mSpecular;
        osg::Vec4f mDiffuse;
        osg::Vec4f mAmbient;
        osg::Vec4f mPosition;
        float mConstantAttenuation = 1.f;
        float mLinearAttenuation = 0.f;
        float mQuadraticAttenuation = 0.f;
    };

    class PPLightBuffer
    {
    public:
        // Lights post-processing shaders get at once (omw_GetPointLight*). The nearest ones in view are chosen each
        // frame; lights joining or leaving the set fade in and out (LightManager::fillPPLights) so a crowded view
        // doesn't make distant lights blink.
        inline static constexpr auto sMaxPPLights = 64;
        inline static constexpr auto sMaxPPLightsArraySize = sMaxPPLights * 3;

        PPLightBuffer()
        {
            for (size_t i = 0; i < 2; ++i)
            {
                mIndex[i] = 0;
                mUniformBuffers[i]
                    = new osg::Uniform(osg::Uniform::FLOAT_VEC4, "omw_PointLights", sMaxPPLightsArraySize);
                mUniformCount[i] = new osg::Uniform("omw_PointLightsCount", static_cast<int>(0));
            }
        }

        void applyUniforms(size_t frame, osg::StateSet* stateset)
        {
            size_t frameId = frame % 2;

            if (!stateset->getUniform("omw_PointLights"))
                stateset->addUniform(mUniformBuffers[frameId]);
            if (!stateset->getUniform("omw_PointLightsCount"))
                stateset->addUniform(mUniformCount[frameId]);

            mUniformBuffers[frameId]->dirty();
            mUniformCount[frameId]->dirty();
        }

        void clear(size_t frame) { mIndex[frame % 2] = 0; }

        void setLight(size_t frame, const Light* light, float radius, float weight = 1.f)
        {
            size_t frameId = frame % 2;
            int i = mIndex[frameId];

            if (i >= sMaxPPLights)
                return;

            i *= 3;

            mUniformBuffers[frameId]->setElement(i + 0, light->getPosition());
            mUniformBuffers[frameId]->setElement(i + 1, light->getDiffuse() * weight);
            mUniformBuffers[frameId]->setElement(i + 2,
                osg::Vec4f(light->getConstantAttenuation(), light->getLinearAttenuation(),
                    light->getQuadraticAttenuation(), radius));

            mIndex[frameId]++;
        }

        void updateCount(size_t frame)
        {
            size_t frameId = frame % 2;
            mUniformCount[frameId]->set(mIndex[frameId]);
        }

    private:
        DoubleBuffer<int> mIndex;
        DoubleBuffer<osg::ref_ptr<osg::Uniform>> mUniformBuffers;
        DoubleBuffer<osg::ref_ptr<osg::Uniform>> mUniformCount;
    };

    /// LightSource managed by a LightManager.
    /// @par Typically used for point lights. Spot lights are not supported yet. Directional lights affect the whole
    /// scene
    ///     so do not need to be managed by a LightManager - so for directional lights use a plain osg::LightSource
    ///     instead.
    /// @note LightSources must be decorated by a LightManager node in order to have an effect. Typical use would
    ///     be one LightManager as the root of the scene graph.
    /// @note One needs to attach LightListCallback's to the scene to have objects receive lighting from LightSources.
    ///     See the documentation of LightListCallback for more information.
    /// @note The position of the contained Light is automatically updated based on the LightSource's world
    /// position.
    class LightSource : public osg::Node
    {
        // double buffered, since one of them may be in use by the draw thread at any given time
        DoubleBuffer<osg::ref_ptr<Light>> mLight;

        // LightSource will affect objects within this radius
        float mRadius;

        int mId;

        float mActorFade;
        float mBounce = -1.f;

        size_t mLastAppliedFrame;

        bool mEmpty = false;

    public:
        META_Node(SceneUtil, LightSource)

        LightSource();

        LightSource(const LightSource& copy, const osg::CopyOp& copyop);

        float getRadius() const { return mRadius; }

        /// The LightSource will affect objects within this radius.
        void setRadius(float radius) { mRadius = radius; }

        void setActorFade(float alpha) { mActorFade = alpha; }

        /// Its own light bounce, instead of the scene's (negative: the scene's). Clustered lighting only.
        void setBounce(float bounce) { mBounce = bounce; }
        float getBounce() const { return mBounce; }

        float getActorFade() const { return mActorFade; }

        void setEmpty(bool empty) { mEmpty = empty; }

        bool getEmpty() const { return mEmpty; }

        /// Get the Light safe for modification in the given frame.
        /// @par May be used externally to animate the light's color/attenuation properties,
        /// and is used internally to synchronize the light's position with the position of the LightSource.
        Light* getLight(size_t frame) { return mLight[frame % 2]; }

        /// @warning It is recommended not to replace an existing Light, because there might still be
        /// references to it in the light StateSet cache that are associated with this LightSource's ID.
        /// These references will stay valid due to ref_ptr but will point to the old object.
        /// @warning Do not modify the \a light after you've called this function.
        void setLight(Light* light)
        {
            mLight[0] = light;
            mLight[1] = new Light(*light);
        }

        /// Get the unique ID for this light source.
        int getId() const { return mId; }

        void setLastAppliedFrame(size_t lastAppliedFrame) { mLastAppliedFrame = lastAppliedFrame; }

        size_t getLastAppliedFrame() const { return mLastAppliedFrame; }
    };

    struct LightSettings
    {
        bool mClusteredLighting = false;
        int mMaxLights = 8;
        float mMaximumLightDistance = Constants::CellSizeInUnits;
        float mLightFadeStart = 0;
        float mLightRadiusMultiplier = 1;
        osg::Vec3i mClusteredGridSize = { 16, 8, 24 };
        int mClusteredWorkGroupSize = 512;
    };

    class LightManagerCullCallback;

    /// OpenMGE XE light occlusion: whether solid world geometry (walls, floors, doors) lies between two world-space
    /// points. Implemented by the game with the physics world.
    class LightOcclusionTest : public osg::Referenced
    {
    public:
        virtual bool isBlocked(const osg::Vec3f& from, const osg::Vec3f& to) = 0;

        /// Counts the moves of things that hide light other than the static world (doors turning).
        virtual unsigned int getChangeCount() const { return 0; }

        /// Appends to @a out the world boxes of the moves after @a count. False if they are no longer known (there were
        /// too many since), when anything may have changed.
        virtual bool getChangesSince(unsigned int count, std::vector<osg::BoundingBox>& out) const
        {
            return count == getChangeCount();
        }
    };

    /// @brief Decorator node implementing the rendering of any number of LightSources that can be anywhere in the
    /// subgraph.
    class LightManager : public osg::Group
    {
    public:
        struct LightSourceTransform
        {
            LightSource* mLightSource;
            osg::Matrixf mWorldMatrix;
        };

        struct LightSourceViewBound
        {
            LightSource* mLightSource;
            osg::BoundingSphere mViewBound;
            bool mCulled = false;
            // Clustered lighting: the light's place in this frame's light buffer for this camera (-1 if not in it).
            // Set by LightManagerCullCallback as it fills the buffer.
            mutable int mGpuIndex = -1;
        };

        using LightList = std::vector<const LightSourceViewBound*>;

        /// What one lit object last found out about one light (LightListCallback keeps one per light it saw).
        struct OcclusionCacheEntry
        {
            const LightSource* mLight = nullptr;
            osg::Vec3f mLightPos;
            osg::Vec3f mObjectPos;
            // frame last tested, and last needed
            size_t mFrame = 0;
            size_t mUsedFrame = 0;
            // LightOcclusionTest::getChangeCount when tested, or when last found clear of the doors moved since
            unsigned int mChangeCount = 0;
            // how much of the object the light reaches (0 to 1), as last tested
            float mVisible = 1.f;
            // changes fade in: from this, starting then
            float mFadeFrom = 1.f;
            double mFadeStart = 0.0;
        };
        using OcclusionCache = std::vector<OcclusionCacheEntry>;
        using SupportedMethods = std::array<bool, 3>;

        META_Node(SceneUtil, LightManager)

        explicit LightManager(
            const LightSettings& settings = LightSettings{}, Resource::ResourceSystem* resourceSystem = nullptr);

        LightManager(const LightManager& copy, const osg::CopyOp& copyop);

        /// @param mask This mask is compared with the current Camera's cull mask to determine if lighting is desired.
        /// By default, it's ~0u i.e. always on.
        /// If you have some views that do not require lighting, then set the Camera's cull mask to not include
        /// the lightingMask for a much faster cull and rendering.
        void setLightingMask(size_t mask);
        size_t getLightingMask() const;

        /// Internal use only, called automatically by the LightManager's UpdateCallback
        void update(size_t frameNum);

        /// Internal use only, called automatically by the LightSource's UpdateCallback
        void addLight(LightSource* lightSource, const osg::Matrixf& worldMat, size_t frameNum);

        const std::vector<LightSourceViewBound>& getLightsInViewSpace(
            osgUtil::CullVisitor* cv, const osg::RefMatrix* viewMatrix, size_t frameNum);

        /// Appends to @a out the lights of getLightsInViewSpace whose view bound intersects @a bound (in the same
        /// camera's view space), except @a ignored ones. The same lights as testing every one, but only those whose
        /// centre is within reach along view x are tested. They come in an order fixed for the frame and camera (the
        /// same lights always in the same order, so equal lists share a state set), not getLightsInViewSpace's.
        void getLightsIntersecting(osgUtil::CullVisitor* cv, const osg::RefMatrix* viewMatrix, size_t frameNum,
            const osg::BoundingSphere& bound, const std::set<LightSource*>& ignored, LightList& out);

        /// OpenMGE XE light occlusion: lights with world geometry between them and the whole of an object stop
        /// lighting it, so a lamp doesn't light the next room through the wall. @a msPerFrame caps the time spent
        /// re-testing results already known; lights and objects not tested yet (just come into view) or that have
        /// moved may use four times as much. Results are kept until the light or the object moves; objects with a
        /// bound radius above @a maxObjectRadius (merged chunks, room shells) are never tested.
        void setLightOcclusion(LightOcclusionTest* test, bool enabled, float msPerFrame, float maxObjectRadius);

        /// Sets @a visibility (one per light of @a lightList) to how much of @a viewBound (in the view space of
        /// @a viewMatrix) each light reaches past the world, from 0 (hidden) to 1, fading over half a second when it
        /// changes. With @a localBox (the object's bounding box, in the space @a modelView takes to view space) the
        /// light is tested against points on the sides of the box facing it instead of points around the bounding
        /// sphere's centre, so large pieces (walls, floors, room parts) next to a light keep it, and a floor that sees
        /// a lamp only through a doorway gets part of its light.
        void getLightVisibility(const osg::RefMatrix* viewMatrix, size_t frameNum, double time,
            const osg::BoundingSphere& viewBound, const LightList& lightList, std::vector<float>& visibility,
            OcclusionCache& cache, const osg::BoundingBox* localBox = nullptr, const osg::Matrix* modelView = nullptr);

        /// Removes from @a lightList the lights the world (nearly) hides from @a viewBound, for the lighting methods
        /// with per-object light lists, which can't light an object with part of a light.
        void removeOccludedLights(const osg::RefMatrix* viewMatrix, size_t frameNum, double time,
            const osg::BoundingSphere& viewBound, LightList& lightList, OcclusionCache& cache,
            const osg::BoundingBox* localBox = nullptr, const osg::Matrix* modelView = nullptr);

        bool getLightOcclusionEnabled() const { return mOcclusionEnabled; }

        /// Clustered lighting has no per-object light lists, so hidden lights are dimmed by the shaders instead: a
        /// state set telling them how much of each light of the light buffer to leave out, 4 bits a light (bits
        /// 4 * (i % 8) of word i / 8 for the light at index i: 0 all of the light reaches the object, 15 none of it).
        static constexpr unsigned int sMaxBlockableLights = 1024;
        static constexpr unsigned int sLightShadeLevels = 15;
        using BlockedLightsMask = std::array<unsigned int, sMaxBlockableLights / 8>;

        osg::ref_ptr<osg::StateSet> getBlockedLightsStateSet(const BlockedLightsMask& mask);

        /// How many blocked-light masks are pushed on the cull visitor's state right now (LightListCallback only).
        int mBlockedMaskDepth = 0;

        osg::ref_ptr<osg::StateSet> getLightListStateSet(
            const LightList& lightList, size_t frameNum, const osg::RefMatrix* viewMatrix);

        void setSunlight(osg::ref_ptr<Light> sun);
        osg::ref_ptr<Light> getSunlight();

        bool getClusteredLighting() const;

        int getMaxLights() const;

        bool isClusteredSupported() { return mSupportsClustered; }

        std::map<std::string, std::string> getLightDefines() const;

        void processChangedSettings(float lightBoundsMultiplier, float maximumLightDistance, float lightFadeStart);

        /// Not thread safe, it is the responsibility of the caller to stop/start threading on the viewer
        void updateMaxLights(int maxLights);

        osg::ref_ptr<osg::Uniform> generateLightBufferUniform();

        // Whether to collect main scene camera points lights into a buffer to be later sent to postprocessing shaders
        void setCollectPPLights(bool enabled);

        std::shared_ptr<PPLightBuffer> getPPLightsBuffer() { return mPPLightBuffer; }

        float getPointLightRadiusMultiplier() const { return mPointLightRadiusMultiplier; }

        float getPointLightFadeEnd() const { return mPointLightFadeEnd; }

        void enableClustered(bool enabled);

        Resource::ResourceSystem* getResourceSystem() { return mResourceSystem; }

    private:
        void initPerObjectUniform(int targetLights);
        void initClustered();

        void updateSettings(float lightBoundsMultiplier, float maximumLightDistance, float lightFadeStart);

        void setMaxLights(int value);

        Resource::ResourceSystem* mResourceSystem;

        std::vector<LightSourceTransform> mLights;

        using LightSourceViewBoundCollection = std::vector<LightSourceViewBound>;

        struct ViewSpaceLights
        {
            LightSourceViewBoundCollection mLights;

            // The view bounds again as flat arrays for getLightsIntersecting, in ascending order of centre x, with each
            // light's index in mLights. Lights reaching much further than the rest are kept apart (the last mNumWide
            // entries, in mLights order), so they don't widen the search along x for every object.
            std::vector<float> mX;
            std::vector<float> mY;
            std::vector<float> mZ;
            std::vector<float> mRadius;
            std::vector<std::uint32_t> mIndex;
            std::size_t mNumWide = 0;
            float mMaxRadius = 0.f;
        };

        ViewSpaceLights& getViewSpaceLights(osgUtil::CullVisitor* cv, const osg::RefMatrix* viewMatrix, size_t frameNum);

        std::map<osg::observer_ptr<osg::Camera>, ViewSpaceLights> mLightsInViewSpace;
        // The entry the last lookup found: every lit object of a view asks for the same one in a row.
        const osg::Camera* mLastViewSpaceCamera = nullptr;
        size_t mLastViewSpaceFrame = 0;
        ViewSpaceLights* mLastViewSpaceLights = nullptr;

        using LightListStateSetKey = std::pair<const osg::RefMatrix*, std::vector<int>>;
        std::map<LightListStateSetKey, osg::ref_ptr<osg::StateSet>> mLightListStateSets;
        // reused for lookups, so a light list already seen this frame costs no allocation
        LightListStateSetKey mLightListStateSetKey;

        std::map<BlockedLightsMask, osg::ref_ptr<osg::StateSet>> mBlockedLightsStateSets;

        size_t mLightingMask;

        osg::ref_ptr<Light> mSun;

        bool mClusteredLighting;

        float mPointLightRadiusMultiplier;
        float mPointLightFadeEnd;
        float mPointLightFadeStart;

        int mMaxLights;

        bool mSupportsClustered;

        std::shared_ptr<PPLightBuffer> mPPLightBuffer;

        // Post-processing lights: how far each light (by id) has faded into the set shaders get, and when it was last
        // seen, so lights joining or leaving the set fade instead of popping.
        struct PPLightFade
        {
            float mWeight = 0.f;
            size_t mFrame = 0;
        };
        std::unordered_map<int, PPLightFade> mPPLightFades;
        double mPPLastTime = -1.0;
        void fillPPLights(const LightSourceViewBoundCollection& collection, size_t frameNum, double time);

        osg::ref_ptr<LightManagerCullCallback> mCullCallback;

        // how much of the object the light reaches, 0 to 1
        float testLightVisibility(const osg::Vec3f& lightPos, const osg::Vec3f& objectPos, float objectRadius);
        float testLightVisibilityFromBox(const osg::Vec3f& lightPos, const osg::BoundingBox& worldBox);
        // whether a door moved since @a entry was tested across the line from the light to the object
        bool changedSince(const OcclusionCacheEntry& entry, const osg::Vec3f& lightPos, const osg::Vec3f& objectPos,
            float objectRadius);

        osg::ref_ptr<LightOcclusionTest> mOcclusionTest;
        bool mOcclusionEnabled = false;
        // per frame, in microseconds: for re-tests of known results, and for pairs not tested yet or moved
        double mOcclusionRetestBudget = 0.0;
        double mOcclusionNewBudget = 0.0;
        float mOcclusionMaxObjectRadius = 0.f;
        size_t mOcclusionFrame = 0;
        double mOcclusionTimeUsed = 0.0;
        unsigned int mOcclusionRaysUsed = 0;
        /// What light occlusion did, summed until it is logged (every few seconds).
        struct OcclusionStats
        {
            size_t mFrames = 0;
            size_t mTests = 0;
            size_t mRays = 0;
            double mTime = 0.0;
            double mWorstFrameTime = 0.0;
            // pairs drawn lit because they could not be tested yet
            size_t mWaiting = 0;
            // re-tests put off for lack of time (the old result stood)
            size_t mPutOff = 0;
            // re-tests of a pair that hadn't moved that came out the other way
            size_t mFlips = 0;
            size_t mBlocked = 0;
        };
        OcclusionStats mOcclusionStats;
        double mOcclusionFrameTime = 0.0;
        unsigned int mOcclusionChangeCount = 0;
        std::vector<osg::BoundingBox> mOcclusionChanges;
        const osg::RefMatrix* mOcclusionInverseViewFor = nullptr;
        size_t mOcclusionInverseViewFrame = 0;
        // in double precision: the boxes and points it gives must not shift as the camera moves
        osg::Matrixd mOcclusionInverseView;
    };

    class LightManagerCullCallback
        : public SceneUtil::NodeCallback<LightManagerCullCallback, LightManager*, osgUtil::CullVisitor*>
    {
    public:
        LightManagerCullCallback(const LightSettings& settings = LightSettings{});

        void operator()(LightManager* node, osgUtil::CullVisitor* cv);

        void reset() { mCache.clear(); }

        struct ViewData
        {
            DoubleBuffer<osg::Matrixd> mProjection;
            DoubleBuffer<osg::ref_ptr<osg::BufferTemplate<std::vector<PointLight>>>> mGPULights;
            DoubleBuffer<osg::ref_ptr<osg::StateSet>> mStateSet;
            DoubleBuffer<osg::ref_ptr<osg::ShaderStorageBufferBinding>> mPointLightSSBB;
            DoubleBuffer<osg::ref_ptr<osg::ShaderStorageBufferBinding>> mLightGridSSBB;
            DoubleBuffer<osg::ref_ptr<osg::ShaderStorageBufferBinding>> mLightIndexListSSBB;
            DoubleBuffer<osg::ref_ptr<osg::ShaderStorageBufferBinding>> mLightIndexCounterSSBB;
            DoubleBuffer<osg::ref_ptr<osg::DispatchCompute>> mClusterComputeNode;
            DoubleBuffer<osg::ref_ptr<osg::DispatchCompute>> mCullComputeNode;
            osg::ref_ptr<osg::ShaderStorageBufferBinding> mClusterSSBB;

            float mClusterFar = 1.f;
            size_t mLastFrameNumber = 0;
        };

        std::unordered_map<osg::Camera*, ViewData> mCache;

        const int mGridSizeX;
        const int mGridSizeY;
        const int mGridSizeZ;
        const int mNumClusters;
        const int mWorkGroupSize;

        const int mMaxLightsPerCluster = 512;
    };

    /// To receive lighting, objects must be decorated by a LightListCallback. Light list callbacks must be added via
    /// node->addCullCallback(new LightListCallback). Once a light list callback is added to a node, that node and all
    /// its child nodes can receive lighting.
    /// @par The placement of these LightListCallbacks affects the granularity of light lists. Having too fine grained
    /// light lists can result in degraded performance. Too coarse grained light lists can result in lights no longer
    /// rendering when the size of a light list exceeds the OpenGL limit on the number of concurrent lights (8). A good
    /// starting point is to attach a LightListCallback to each game object's base node.
    /// @note Not thread safe for CullThreadPerCamera threading mode.
    /// @note Due to lack of OSG support, the callback does not work on Drawables.
    class LightListCallback : public SceneUtil::NodeCallback<LightListCallback, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        LightListCallback()
            : mLightManager(nullptr)
            , mLastFrameNumber(0)
        {
        }
        LightListCallback(const LightListCallback& copy, const osg::CopyOp& copyop)
            : osg::Object(copy, copyop)
            , SceneUtil::NodeCallback<LightListCallback, osg::Node*, osgUtil::CullVisitor*>(copy, copyop)
            , mLightManager(copy.mLightManager)
            , mLastFrameNumber(0)
            , mIgnoredLightSources(copy.mIgnoredLightSources)
        {
        }

        META_Object(SceneUtil, LightListCallback)

        void operator()(osg::Node* node, osgUtil::CullVisitor* nv);

        bool pushLightState(osg::Node* node, osgUtil::CullVisitor* nv);

        /// The node's bounding box in the space the cull visitor's model view matrix takes to view space, for light
        /// occlusion of large objects; nullptr for small ones. Kept until the node's bound changes.
        const osg::BoundingBox* getOcclusionBox(osg::Node* node);

        /// Clustered lighting with light occlusion: pushes the mask of lights the world hides from @a node, if any.
        /// @return 0 if nothing was pushed, 1 for an empty mask (undoing a parent's), 2 for a mask with lights in it.
        int pushBlockedLightsState(osg::Node* node, osgUtil::CullVisitor* cv);

        std::set<SceneUtil::LightSource*>& getIgnoredLightSources() { return mIgnoredLightSources; }

    private:
        LightManager* mLightManager;
        size_t mLastFrameNumber;
        LightManager::LightList mLightList;
        std::vector<float> mLightVisibility;
        std::set<SceneUtil::LightSource*> mIgnoredLightSources;
        LightManager::OcclusionCache mOcclusionCache;
        // light occlusion: the node's box (see getOcclusionBox) and the bound it was made for
        osg::BoundingBox mOcclusionBox;
        osg::BoundingSphere mOcclusionBoxFor;
        bool mHasOcclusionBox = false;
        // clustered lighting: the mask found for the last camera and frame
        const osg::Camera* mBlockedCamera = nullptr;
        size_t mBlockedFrame = 0;
        osg::ref_ptr<osg::StateSet> mBlockedStateSet;
        bool mBlockedAny = false;
    };

    void configureStateSetSunOverride(const Light* light, osg::StateSet* stateset,
        int mode = osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);

    void configureSunAmbientOverride(const osg::Vec4f& ambient, osg::StateSet* stateset);
}

#endif
