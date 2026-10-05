#include "water.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <span>
#include <sstream>
#include <tuple>

#include <osg/ClipNode>
#include <osg/Depth>
#include <osg/Fog>
#include <osg/FrontFace>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/PositionAttitudeTransform>
#include <osg/Texture2D>
#include <osg/ViewportIndexed>

#include <osgUtil/CullVisitor>
#include <osgUtil/IncrementalCompileOperation>

#include <components/resource/imagemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>

#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/fog.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/rtt.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/sceneutil/waterutil.hpp>

#include <components/misc/constants.hpp>
#include <components/stereo/stereomanager.hpp>

#include <components/nifosg/controller.hpp>

#include <components/shader/shadermanager.hpp>

#include <components/esm/util.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esmterrain/storage.hpp>

#include <components/fallback/fallback.hpp>

#include <components/settings/values.hpp>

#include "../mwworld/cellstore.hpp"

#include "renderbin.hpp"
#include "ripples.hpp"
#include "ripplesimulation.hpp"
#include "util.hpp"
#include "vismask.hpp"

namespace MWRender
{

    namespace
    {
        // OpenMGE XE 3D water
        constexpr int sWaveGridQuads = 256; // per side
        constexpr float sWaveGridSpacing = 48.f;
        constexpr float sWaveGridHalfSize = sWaveGridQuads * sWaveGridSpacing / 2; // 6144
        constexpr int sWaveDepthMapSize = 128; // texels per side
        constexpr float sWaveDepthMapTexel = 128.f; // one terrain vertex: the map covers 16384 units
        constexpr float sWaveFullDepth = 192.f; // water depth at which waves reach their full height
        constexpr int sWaveDepthMapUnit = 5;
    }

    // --------------------------------------------------------------------------------------------------------------------------------

    /// @brief Allows to cull and clip meshes that are below a plane. Useful for reflection camera effects.
    /// Also handles flipping of the plane when the eye point goes below it.
    /// To use, simply create the scene as subgraph of this node, then do setPlane(const osg::Plane& plane);
    class ClipCullNode : public osg::Group
    {
        class PlaneCullCallback : public SceneUtil::NodeCallback<PlaneCullCallback, osg::Node*, osgUtil::CullVisitor*>
        {
        public:
            /// @param cullPlane The culling plane (in world space).
            PlaneCullCallback(const osg::Plane* cullPlane)
                : mCullPlane(cullPlane)
            {
            }

            void operator()(osg::Node* node, osgUtil::CullVisitor* cv)
            {
                osg::Polytope::PlaneList origPlaneList
                    = cv->getProjectionCullingStack().back().getFrustum().getPlaneList();

                osg::Plane plane = *mCullPlane;
                plane.transform(*cv->getCurrentRenderStage()->getInitialViewMatrix());

                osg::Vec3d eyePoint = cv->getEyePoint();
                if (mCullPlane->intersect(osg::BoundingSphere(osg::Vec3d(0, 0, eyePoint.z()), 0)) > 0)
                    plane.flip();

                cv->getProjectionCullingStack().back().getFrustum().add(plane);

                traverse(node, cv);

                // undo
                cv->getProjectionCullingStack().back().getFrustum().set(origPlaneList);
            }

        private:
            const osg::Plane* mCullPlane;
        };

        class FlipCallback : public SceneUtil::NodeCallback<FlipCallback, osg::Node*, osgUtil::CullVisitor*>
        {
        public:
            FlipCallback(const osg::Plane* cullPlane)
                : mCullPlane(cullPlane)
            {
            }

            void operator()(osg::Node* node, osgUtil::CullVisitor* cv)
            {
                osg::Vec3d eyePoint = cv->getEyePoint();

                osg::RefMatrix* modelViewMatrix = new osg::RefMatrix(*cv->getModelViewMatrix());

                // apply the height of the plane
                // we can't apply this height in the addClipPlane() since the "flip the below graph" function would
                // otherwise flip the height as well
                modelViewMatrix->preMultTranslate(mCullPlane->getNormal() * ((*mCullPlane)[3] * -1));

                // flip the below graph if the eye point is above the plane
                if (mCullPlane->intersect(osg::BoundingSphere(osg::Vec3d(0, 0, eyePoint.z()), 0)) > 0)
                {
                    modelViewMatrix->preMultScale(osg::Vec3(1, 1, -1));
                }

                // move the plane back along its normal a little bit to prevent bleeding at the water shore
                const float fov = Settings::camera().mFieldOfView;
                constexpr double clipFudgeMin = 2.5; // minimum offset of clip plane
                constexpr double clipFudgeScale = -15000.0;
                double clipFudge
                    = std::abs(std::abs((*mCullPlane)[3]) - eyePoint.z()) * fov / clipFudgeScale - clipFudgeMin;
                modelViewMatrix->preMultTranslate(mCullPlane->getNormal() * clipFudge);

                cv->pushModelViewMatrix(modelViewMatrix, osg::Transform::RELATIVE_RF);
                traverse(node, cv);
                cv->popModelViewMatrix();
            }

        private:
            const osg::Plane* mCullPlane;
        };

    public:
        ClipCullNode()
        {
            addCullCallback(new PlaneCullCallback(&mPlane));

            mClipNodeTransform = new osg::Group;
            mClipNodeTransform->addCullCallback(new FlipCallback(&mPlane));
            osg::Group::addChild(mClipNodeTransform);

            mClipNode = new osg::ClipNode;

            mClipNodeTransform->addChild(mClipNode);
        }

        void setPlane(const osg::Plane& plane)
        {
            if (plane == mPlane)
                return;
            mPlane = plane;

            mClipNode->getClipPlaneList().clear();
            mClipNode->addClipPlane(
                new osg::ClipPlane(0, osg::Plane(mPlane.getNormal(), 0))); // mPlane.d() applied in FlipCallback
            mClipNode->setStateSetModes(*getOrCreateStateSet(), osg::StateAttribute::ON);
            mClipNode->setCullingActive(false);
        }

    private:
        osg::ref_ptr<osg::Group> mClipNodeTransform;
        osg::ref_ptr<osg::ClipNode> mClipNode;

        osg::Plane mPlane;
    };

    /// This callback on the Camera has the effect of a RELATIVE_RF_INHERIT_VIEWPOINT transform mode (which does not
    /// exist in OSG). We want to keep the View Point of the parent camera so we will not have to recreate LODs.
    class InheritViewPointCallback
        : public SceneUtil::NodeCallback<InheritViewPointCallback, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        InheritViewPointCallback() {}

        void operator()(osg::Node* node, osgUtil::CullVisitor* cv)
        {
            osg::ref_ptr<osg::RefMatrix> modelViewMatrix = new osg::RefMatrix(*cv->getModelViewMatrix());
            cv->popModelViewMatrix();
            cv->pushModelViewMatrix(modelViewMatrix, osg::Transform::ABSOLUTE_RF_INHERIT_VIEWPOINT);
            traverse(node, cv);
        }
    };

    /// Moves water mesh away from the camera slightly if the camera gets too close on the Z axis.
    /// The offset works around graphics artifacts that occurred with the GL_DEPTH_CLAMP when the camera gets extremely
    /// close to the mesh (seen on NVIDIA at least). Must be added as a Cull callback.
    class FudgeCallback : public SceneUtil::NodeCallback<FudgeCallback, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        void operator()(osg::Node* node, osgUtil::CullVisitor* cv)
        {
            const float fudge = 0.2f;
            if (std::abs(cv->getEyeLocal().z()) < fudge)
            {
                float diff = fudge - cv->getEyeLocal().z();
                osg::RefMatrix* modelViewMatrix = new osg::RefMatrix(*cv->getModelViewMatrix());

                if (cv->getEyeLocal().z() >= 0)
                    modelViewMatrix->preMultTranslate(osg::Vec3f(0, 0, -diff));
                else
                    modelViewMatrix->preMultTranslate(osg::Vec3f(0, 0, diff));

                cv->pushModelViewMatrix(modelViewMatrix, osg::Transform::RELATIVE_RF);
                traverse(node, cv);
                cv->popModelViewMatrix();
            }
            else
                traverse(node, cv);
        }
    };

    class RainSettingsUpdater : public SceneUtil::StateSetUpdater
    {
    public:
        RainSettingsUpdater() = default;

        void setRainIntensity(float rainIntensity) { mRainIntensity = rainIntensity; }

    protected:
        void setDefaults(osg::StateSet* stateset) override
        {
            osg::ref_ptr<osg::Uniform> rainIntensityUniform = new osg::Uniform("rainIntensity", 0.0f);
            stateset->addUniform(rainIntensityUniform.get());
        }

        void apply(osg::StateSet* stateset, osg::NodeVisitor* /*nv*/) override
        {
            osg::ref_ptr<osg::Uniform> rainIntensityUniform = stateset->getUniform("rainIntensity");
            if (rainIntensityUniform != nullptr)
                rainIntensityUniform->set(mRainIntensity);
        }

    private:
        float mRainIntensity{ 0.f };
    };

    class Reflection : public SceneUtil::RTTNode
    {
    public:
        Reflection(uint32_t rttSize, bool isInterior)
            : RTTNode(rttSize, rttSize, 0, false, 0, StereoAwareness::Aware, shouldAddMSAAIntermediateTarget())
        {
            setInterior(isInterior);
            setDepthBufferInternalFormat(GL_DEPTH32F_STENCIL8);
            setUpdateInterval(static_cast<unsigned int>(Settings::water().mReflectionUpdateInterval.get()));
            mClipCullNode = new ClipCullNode;
        }

        void setDefaults(osg::Camera* camera) override
        {
            camera->setReferenceFrame(osg::Camera::RELATIVE_RF);
            camera->setSmallFeatureCullingPixelSize(Settings::water().mSmallFeatureCullingPixelSize);
            camera->setName(Constants::ReflectionCamera);
            camera->addCullCallback(new InheritViewPointCallback);

            // Inform the shader that we're in a reflection
            camera->getOrCreateStateSet()->addUniform(new osg::Uniform("isReflection", true));

            // XXX: should really flip the FrontFace on each renderable instead of forcing clockwise.
            osg::ref_ptr<osg::FrontFace> frontFace(new osg::FrontFace);
            frontFace->setMode(osg::FrontFace::CLOCKWISE);
            camera->getOrCreateStateSet()->setAttributeAndModes(frontFace, osg::StateAttribute::ON);

            camera->addChild(mClipCullNode);
            camera->setNodeMask(Mask_RenderToTexture);

            SceneUtil::ShadowManager::instance().disableShadowsForStateSet(*camera->getOrCreateStateSet());
        }

        void apply(osg::Camera* camera) override
        {
            camera->setViewMatrix(mViewMatrix);
            camera->setCullMask(mNodeMask);
        }

        void setInterior(bool isInterior)
        {
            mInterior = isInterior;
            mNodeMask = calcNodeMask();
        }

        void setWaterLevel(float waterLevel)
        {
            mViewMatrix = osg::Matrix::scale(1, 1, -1) * osg::Matrix::translate(0, 0, 2 * waterLevel);
            mClipCullNode->setPlane(osg::Plane(osg::Vec3d(0, 0, 1), osg::Vec3d(0, 0, waterLevel)));
        }

        void setScene(osg::Node* scene)
        {
            if (mScene)
                mClipCullNode->removeChild(mScene);
            mScene = scene;
            mClipCullNode->addChild(scene);
        }

        void showWorld(bool show)
        {
            if (show)
                mNodeMask = calcNodeMask();
            else
                mNodeMask = calcNodeMask() & ~sToggleWorldMask;
        }

    private:
        unsigned int calcNodeMask()
        {
            int reflectionDetail = Settings::water().mReflectionDetail;
            reflectionDetail = std::clamp(reflectionDetail, mInterior ? 2 : 0, 5);
            unsigned int extraMask = 0;
            if (reflectionDetail >= 1)
                extraMask |= Mask_Terrain;
            if (reflectionDetail >= 2)
                extraMask |= Mask_Static;
            if (reflectionDetail >= 3)
                extraMask |= Mask_Effect | Mask_ParticleSystem | Mask_Object;
            if (reflectionDetail >= 4)
                extraMask |= Mask_Player | Mask_Actor;
            if (reflectionDetail >= 5)
                extraMask |= Mask_Groundcover;
            return Mask_Scene | Mask_Sky | Mask_Lighting | extraMask;
        }

        osg::ref_ptr<ClipCullNode> mClipCullNode;
        osg::ref_ptr<osg::Node> mScene;
        osg::Node::NodeMask mNodeMask;
        osg::Matrix mViewMatrix{ osg::Matrix::identity() };
        bool mInterior;
    };

    /// DepthClampCallback enables GL_DEPTH_CLAMP for the current draw, if supported.
    class DepthClampCallback : public osg::Drawable::DrawCallback
    {
    public:
        void drawImplementation(osg::RenderInfo& renderInfo, const osg::Drawable* drawable) const override
        {
            static bool supported = osg::isGLExtensionOrVersionSupported(
                renderInfo.getState()->getContextID(), "GL_ARB_depth_clamp", 3.3f);
            if (!supported)
            {
                drawable->drawImplementation(renderInfo);
                return;
            }

            glEnable(GL_DEPTH_CLAMP);

            drawable->drawImplementation(renderInfo);

            // restore default
            glDisable(GL_DEPTH_CLAMP);
        }
    };

    Water::Water(osg::Group* parent, osg::Group* sceneRoot, Resource::ResourceSystem* resourceSystem,
        osgUtil::IncrementalCompileOperation* ico)
        : mRainSettingsUpdater(nullptr)
        , mParent(parent)
        , mSceneRoot(sceneRoot)
        , mResourceSystem(resourceSystem)
        , mEnabled(true)
        , mToggled(true)
        , mTop(0)
        , mInterior(false)
        , mShowWorld(true)
        , mCullCallback(nullptr)
        , mShaderWaterStateSetUpdater(nullptr)
    {
        mSimulation = std::make_unique<RippleSimulation>(mSceneRoot, resourceSystem);

        mWaterGeom = SceneUtil::createWaterGeometry(Constants::CellSizeInUnits * 150, 40, 900);
        mWaterGeom->setDrawCallback(new DepthClampCallback);
        mWaterGeom->setNodeMask(Mask_Water);
        mWaterGeom->setDataVariance(osg::Object::STATIC);
        mWaterGeom->setName("Water Geometry");

        mWaterNode = new osg::PositionAttitudeTransform;
        mWaterNode->setName("Water Root");
        mWaterNode->addChild(mWaterGeom);
        mWaterNode->addCullCallback(new FudgeCallback);

        // simple water fallback for the local map
        osg::ref_ptr<osg::Geometry> geom2(osg::clone(mWaterGeom.get(), osg::CopyOp::DEEP_COPY_NODES));
        createSimpleWaterStateSet(geom2, Fallback::Map::getFloat("Water_Map_Alpha"));
        geom2->setNodeMask(Mask_SimpleWater);
        geom2->setName("Simple Water Geometry");
        mWaterNode->addChild(geom2);

        mSceneRoot->addChild(mWaterNode);

        setHeight(mTop);

        updateWaterMaterial();

        if (ico)
            ico->add(mWaterNode);
    }

    void Water::setCullCallback(osg::Callback* callback)
    {
        if (mCullCallback)
        {
            mWaterNode->removeCullCallback(mCullCallback);
            if (mReflection)
                mReflection->removeCullCallback(mCullCallback);
        }

        mCullCallback = callback;

        if (callback)
        {
            mWaterNode->addCullCallback(callback);
            if (mReflection)
                mReflection->addCullCallback(callback);
        }
    }

    void Water::updateWaterMaterial()
    {
        if (mShaderWaterStateSetUpdater)
        {
            mWaterNode->removeCullCallback(mShaderWaterStateSetUpdater);
            mShaderWaterStateSetUpdater = nullptr;
        }
        if (mReflection)
        {
            mParent->removeChild(mReflection);
            mReflection = nullptr;
        }
        if (mRipples)
        {
            mParent->removeChild(mRipples);
            mRipples = nullptr;
            mSimulation->setRipples(nullptr);
        }

        mWaterNode->setStateSet(nullptr);
        mWaterGeom->setStateSet(nullptr);
        mWaterGeom->setUpdateCallback(nullptr);

        if (mWaveGrid)
        {
            mWaterNode->removeChild(mWaveGrid);
            mWaveGrid = nullptr;
        }
        mWaveHeight = Settings::water().mShader ? std::max(0.f, Settings::water().mWaveHeight.get()) : 0.f;
        if (mWaveHeight > 0.f)
            createWaveGrid();
        else
            mWaveAmplitude = 0.f;

        if (Settings::water().mShader)
        {
            const unsigned int rttSize = Settings::water().mRttSize;

            mReflection = new Reflection(rttSize, mInterior);
            mReflection->setWaterLevel(mTop);
            mReflection->setScene(mSceneRoot);
            if (mCullCallback)
                mReflection->addCullCallback(mCullCallback);
            mParent->addChild(mReflection);

            mRipples = new Ripples(mResourceSystem);
            mSimulation->setRipples(mRipples);
            mParent->addChild(mRipples);

            showWorld(mShowWorld);

            createShaderWaterStateSet(mWaterNode);
        }
        else
            createSimpleWaterStateSet(mWaterGeom, Fallback::Map::getFloat("Water_World_Alpha"));

        mResourceSystem->getSceneManager()->setUpNormalsRTForStateSet(mWaterGeom->getOrCreateStateSet(), true);
        if (mWaveGrid)
            mResourceSystem->getSceneManager()->setUpNormalsRTForStateSet(mWaveGrid->getOrCreateStateSet(), true);

        updateVisible();
    }

    void Water::createWaveGrid()
    {
        const int n = sWaveGridQuads;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->reserve(static_cast<std::size_t>((n + 1) * (n + 1)));
        for (int y = 0; y <= n; ++y)
            for (int x = 0; x <= n; ++x)
                vertices->push_back(osg::Vec3f((x - n / 2) * sWaveGridSpacing, (y - n / 2) * sWaveGridSpacing, 0.f));

        // Triangles from the outermost ring of quads inwards. Water doesn't write depth, so with the camera near the
        // middle this draws the far waves before the near ones and a near crest covers the trough behind it.
        std::vector<std::tuple<int, int, int>> quads; // ring, x, y
        quads.reserve(static_cast<std::size_t>(n * n));
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            {
                const int dx = x < n / 2 ? n / 2 - 1 - x : x - n / 2;
                const int dy = y < n / 2 ? n / 2 - 1 - y : y - n / 2;
                quads.emplace_back(std::max(dx, dy), x, y);
            }
        std::stable_sort(
            quads.begin(), quads.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
        osg::ref_ptr<osg::DrawElementsUInt> indices = new osg::DrawElementsUInt(GL_TRIANGLES);
        indices->reserve(quads.size() * 6);
        for (const auto& [ring, x, y] : quads)
        {
            const unsigned int i00 = static_cast<unsigned int>(y * (n + 1) + x);
            const unsigned int i10 = i00 + 1;
            const unsigned int i01 = i00 + static_cast<unsigned int>(n + 1);
            const unsigned int i11 = i01 + 1;
            indices->push_back(i00);
            indices->push_back(i10);
            indices->push_back(i11);
            indices->push_back(i00);
            indices->push_back(i11);
            indices->push_back(i01);
        }

        mWaveGrid = new osg::Geometry;
        mWaveGrid->setVertexArray(vertices);
        osg::ref_ptr<osg::Vec3Array> normal = new osg::Vec3Array;
        normal->push_back(osg::Vec3f(0, 0, 1));
        mWaveGrid->setNormalArray(normal, osg::Array::BIND_OVERALL);
        mWaveGrid->addPrimitiveSet(indices);
        mWaveGrid->setUseDisplayList(false);
        mWaveGrid->setUseVertexBufferObjects(true);
        // the shader moves it along with the camera: never culled by its (origin-centred) bounds
        mWaveGrid->setCullingActive(false);
        mWaveGrid->setDrawCallback(new DepthClampCallback);
        mWaveGrid->setNodeMask(0); // shown by updateWaves while there are waves
        mWaveGrid->setDataVariance(osg::Object::STATIC);
        mWaveGrid->setName("Water Wave Grid");
        mWaveGrid->getOrCreateStateSet()->addUniform(new osg::Uniform("waveSurface", true));
        mWaterNode->addChild(mWaveGrid);

        if (!mWaveDepthMap)
        {
            mWaveDepthImage = new osg::Image;
            mWaveDepthImage->allocateImage(sWaveDepthMapSize, sWaveDepthMapSize, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE);
            std::fill_n(mWaveDepthImage->data(), sWaveDepthMapSize * sWaveDepthMapSize, 255);
            mWaveDepthImage->setInternalTextureFormat(GL_LUMINANCE8);
            mWaveDepthMap = new osg::Texture2D(mWaveDepthImage);
            mWaveDepthMap->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
            mWaveDepthMap->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
            mWaveDepthMap->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            mWaveDepthMap->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            mWaveDepthMap->setResizeNonPowerOfTwoHint(false);
            mWaveDepthMap->setUnRefImageDataAfterApply(false);
            mWaveDepthMap->setDataVariance(osg::Object::DYNAMIC);
            mWaveDepthMapValid = false;
        }
    }

    void Water::rebuildWaveDepthMap(const osg::Vec2f& center)
    {
        const float extent = sWaveDepthMapSize * sWaveDepthMapTexel;
        const osg::Vec2f origin(std::floor((center.x() - extent / 2) / sWaveDepthMapTexel) * sWaveDepthMapTexel,
            std::floor((center.y() - extent / 2) / sWaveDepthMapTexel) * sWaveDepthMapTexel);
        unsigned char* data = mWaveDepthImage->data();

        const float cellSize = static_cast<float>(ESM::getCellSize(mWorldspace));
        int cachedCellX = INT_MIN;
        int cachedCellY = INT_MIN;
        std::shared_ptr<const ESMTerrain::LandObject> land;
        const ESM::LandData* landData = nullptr;
        for (int y = 0; y < sWaveDepthMapSize; ++y)
        {
            for (int x = 0; x < sWaveDepthMapSize; ++x)
            {
                const float wx = origin.x() + (x + 0.5f) * sWaveDepthMapTexel;
                const float wy = origin.y() + (y + 0.5f) * sWaveDepthMapTexel;
                float height = ESM::Land::DEFAULT_HEIGHT;
                if (mTerrainStorage != nullptr)
                {
                    const int cellX = static_cast<int>(std::floor(wx / cellSize));
                    const int cellY = static_cast<int>(std::floor(wy / cellSize));
                    if (cellX != cachedCellX || cellY != cachedCellY)
                    {
                        land = mTerrainStorage->getLand(ESM::ExteriorCellLocation(cellX, cellY, mWorldspace));
                        landData = land ? land->getData(ESM::Land::DATA_VHGT) : nullptr;
                        cachedCellX = cellX;
                        cachedCellY = cellY;
                    }
                    if (landData != nullptr)
                    {
                        // bilinear over the cell's height grid
                        const int landSize = landData->getLandSize();
                        const std::span<const float> heights = landData->getHeights();
                        const float fx = std::clamp((wx - cellX * cellSize) / cellSize, 0.f, 1.f) * (landSize - 1);
                        const float fy = std::clamp((wy - cellY * cellSize) / cellSize, 0.f, 1.f) * (landSize - 1);
                        const int x0 = std::min(static_cast<int>(fx), landSize - 2);
                        const int y0 = std::min(static_cast<int>(fy), landSize - 2);
                        const float tx = fx - x0;
                        const float ty = fy - y0;
                        const float h00 = heights[y0 * landSize + x0];
                        const float h10 = heights[y0 * landSize + x0 + 1];
                        const float h01 = heights[(y0 + 1) * landSize + x0];
                        const float h11 = heights[(y0 + 1) * landSize + x0 + 1];
                        height = (h00 * (1 - tx) + h10 * tx) * (1 - ty) + (h01 * (1 - tx) + h11 * tx) * ty;
                    }
                }
                const float depth = std::clamp((mTop - height) / sWaveFullDepth, 0.f, 1.f);
                data[y * sWaveDepthMapSize + x] = static_cast<unsigned char>(depth * 255.f + 0.5f);
            }
        }
        mWaveDepthImage->dirty();
        mWaveDepthMapOrigin = origin;
        mWaveDepthMapWaterLevel = mTop;
        mWaveDepthMapValid = true;
    }

    void Water::updateWaves(const osg::Vec3f& playerPos, float windSpeed)
    {
        if (!mWaveGrid)
            return;
        // calm (clear, foggy) about a third of the height, rain about two thirds, storms full
        const float weather = 0.3f + 0.7f * std::clamp(windSpeed / 0.6f, 0.f, 1.f);
        mWaveAmplitude = mInterior ? 0.f : mWaveHeight * weather;
        mWaveGrid->setNodeMask(mWaveAmplitude > 0.f ? Mask_Water : 0u);
        if (mWaveAmplitude <= 0.f)
            return;

        // Keep the depth map centred near the player; the grid (half size 6144) stays inside it while the player
        // is within 1536 of its centre (half size 8192, a little extra for the third-person camera).
        const float extent = sWaveDepthMapSize * sWaveDepthMapTexel;
        const osg::Vec2f center = mWaveDepthMapOrigin + osg::Vec2f(extent / 2, extent / 2);
        if (!mWaveDepthMapValid || mWaveDepthMapWaterLevel != mTop
            || std::max(std::abs(playerPos.x() - center.x()), std::abs(playerPos.y() - center.y())) > 1536.f)
            rebuildWaveDepthMap(osg::Vec2f(playerPos.x(), playerPos.y()));
    }

    osg::Vec3d Water::getPosition() const
    {
        return mWaterNode->getPosition();
    }

    osg::Drawable* Water::getDrawable() const
    {
        return mWaterGeom;
    }

    void Water::createSimpleWaterStateSet(osg::Node* node, float alpha)
    {
        osg::ref_ptr<osg::StateSet> stateset
            = SceneUtil::createSimpleWaterStateSet(alpha, MWRender::RenderBin_DepthSorted);

        node->setStateSet(stateset);
        node->setUpdateCallback(nullptr);
        mRainSettingsUpdater = nullptr;

        // Add animated textures
        std::vector<osg::ref_ptr<osg::Texture2D>> textures;
        const int frameCount = std::clamp(Fallback::Map::getInt("Water_SurfaceFrameCount"), 0, 320);
        std::string_view texture = Fallback::Map::getString("Water_SurfaceTexture");
        for (int i = 0; i < frameCount; ++i)
        {
            std::ostringstream texname;
            texname << "textures/water/" << texture << std::setw(2) << std::setfill('0') << i << ".dds";
            const VFS::Path::Normalized path(texname.str());
            osg::ref_ptr<osg::Texture2D> tex(new osg::Texture2D(mResourceSystem->getImageManager()->getImage(path)));
            tex->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
            tex->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
            mResourceSystem->getSceneManager()->applyFilterSettings(tex);
            textures.push_back(tex);
        }

        if (textures.empty())
            return;

        float fps = Fallback::Map::getFloat("Water_SurfaceFPS");

        osg::ref_ptr<NifOsg::FlipController> controller(new NifOsg::FlipController(0, 1.f / fps, textures));
        controller->setSource(std::make_shared<SceneUtil::FrameTimeSource>());
        node->setUpdateCallback(controller);

        stateset->setTextureAttribute(0, textures[0], osg::StateAttribute::ON);

        // use a shader to render the simple water, ensuring that fog is applied per pixel as required.
        // this could be removed if a more detailed water mesh, using some sort of paging solution, is implemented.
        Resource::SceneManager* sceneManager = mResourceSystem->getSceneManager();
        sceneManager->recreateShaders(node);
    }

    class ShaderWaterStateSetUpdater : public SceneUtil::StateSetUpdater
    {
    public:
        ShaderWaterStateSetUpdater(Water* water, Resource::ResourceSystem* resourceSystem, Reflection* reflection,
            Ripples* ripples, osg::ref_ptr<osg::Program> program, osg::ref_ptr<osg::Texture2D> normalMap)
            : mWater(water)
            , mReflection(reflection)
            , mRipples(ripples)
            , mProgram(std::move(program))
            , mNormalMap(std::move(normalMap))
            , mResourceSystem(resourceSystem)
            , mOpaqueDepthTextureUnit(resourceSystem->getSceneManager()->getShaderManager().reserveGlobalTextureUnits(
                  Shader::ShaderManager::Slot::OpaqueDepthTexture))
            , mOpaqueColorTextureUnit(resourceSystem->getSceneManager()->getShaderManager().reserveGlobalTextureUnits(
                  Shader::ShaderManager::Slot::OpaqueColorTexture))
        {
        }

        void setDefaults(osg::StateSet* stateset) override
        {
            stateset->addUniform(new osg::Uniform("normalMap", 0));
            stateset->setTextureAttribute(0, mNormalMap, osg::StateAttribute::ON);
            stateset->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
            stateset->setAttributeAndModes(mProgram, osg::StateAttribute::ON);

            stateset->addUniform(new osg::Uniform("reflectionMap", 1));
            stateset->addUniform(new osg::Uniform("opaqueColorTex", mOpaqueColorTextureUnit));
            stateset->addUniform(new osg::Uniform("opaqueDepthTex", mOpaqueDepthTextureUnit));
            stateset->setMode(GL_BLEND, osg::StateAttribute::ON);
            stateset->addUniform(new osg::Uniform("waterSurface", true));
            stateset->setRenderBinDetails(MWRender::RenderBin_DepthSorted, "DepthSortedBin");
            osg::ref_ptr<osg::Depth> depth = new SceneUtil::AutoDepth;
            depth->setWriteMask(false);
            stateset->setAttributeAndModes(depth, osg::StateAttribute::ON);

            if (mRipples)
            {
                stateset->addUniform(new osg::Uniform("rippleMap", 4));
            }
            stateset->addUniform(new osg::Uniform("nodePosition", osg::Vec3f(mWater->getPosition())));

            // 3D water (the wave grid's own state set sets waveSurface)
            stateset->addUniform(new osg::Uniform("waveSurface", false));
            stateset->addUniform(new osg::Uniform("waveGridOffset", osg::Vec2f()));
            stateset->addUniform(new osg::Uniform("waveAmplitude", 0.f));
            stateset->addUniform(new osg::Uniform("waveDepthMapRect", osg::Vec4f()));
            if (osg::Texture2D* depthMap = mWater->getWaveDepthMap())
            {
                stateset->setTextureAttribute(sWaveDepthMapUnit, depthMap, osg::StateAttribute::ON);
                stateset->addUniform(new osg::Uniform("waveDepthMap", sWaveDepthMapUnit));
            }
        }

        void apply(osg::StateSet* stateset, osg::NodeVisitor* nv) override
        {
            osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(nv);
            stateset->setTextureAttribute(1, mReflection->getColorTexture(cv), osg::StateAttribute::ON);
            stateset->setTextureAttribute(mOpaqueColorTextureUnit,
                mResourceSystem->getSceneManager()->getOpaqueColorTex(cv->getTraversalNumber()),
                osg::StateAttribute::ON);
            stateset->setTextureAttribute(mOpaqueDepthTextureUnit,
                mResourceSystem->getSceneManager()->getOpaqueDepthTex(cv->getTraversalNumber()),
                osg::StateAttribute::ON);

            if (mRipples)
            {
                stateset->setTextureAttribute(4, mRipples->getColorTexture(), osg::StateAttribute::ON);
            }
            stateset->getUniform("nodePosition")->set(osg::Vec3f(mWater->getPosition()));

            // The wave grid follows the camera in whole grid steps, so its vertices stay put in the world.
            const osg::Vec3f eye = cv->getEyeLocal();
            const osg::Vec2f gridOffset(std::floor(eye.x() / sWaveGridSpacing + 0.5f) * sWaveGridSpacing,
                std::floor(eye.y() / sWaveGridSpacing + 0.5f) * sWaveGridSpacing);
            stateset->getUniform("waveGridOffset")->set(gridOffset);
            stateset->getUniform("waveAmplitude")->set(mWater->getWaveAmplitude());
            const osg::Vec2f mapOrigin = mWater->getWaveDepthMapOrigin();
            stateset->getUniform("waveDepthMapRect")
                ->set(osg::Vec4f(mapOrigin.x(), mapOrigin.y(), 1.f / (sWaveDepthMapSize * sWaveDepthMapTexel), 0.f));
        }

    private:
        Water* mWater;
        Reflection* mReflection;
        Ripples* mRipples;
        osg::ref_ptr<osg::Program> mProgram;
        osg::ref_ptr<osg::Texture2D> mNormalMap;
        Resource::ResourceSystem* mResourceSystem;
        int mOpaqueDepthTextureUnit;
        int mOpaqueColorTextureUnit;
    };

    void Water::createShaderWaterStateSet(osg::Node* node)
    {
        // use a define map to conditionally compile the shader
        std::map<std::string, std::string> defineMap;
        const int rippleDetail = Settings::water().mRainRippleDetail;
        defineMap["waterRefraction"] = std::string(Settings::water().mRefraction ? "1" : "0");
        defineMap["rainRippleDetail"] = std::to_string(rippleDetail);
        defineMap["rippleMapWorldScale"] = std::to_string(RipplesSurface::sWorldScaleFactor);
        defineMap["rippleMapSize"] = std::to_string(RipplesSurface::sRTTSize) + ".0";
        defineMap["sunlightScattering"] = Settings::water().mSunlightScattering ? "1" : "0";
        defineMap["wobblyShores"] = Settings::water().mWobblyShores ? "1" : "0";
        defineMap["waves"] = mWaveGrid ? "1" : "0";
        defineMap["waveGridHalfSize"] = std::to_string(sWaveGridHalfSize);
        defineMap["waveFullDepth"] = std::to_string(sWaveFullDepth);

        Stereo::shaderStereoDefines(defineMap);

        Shader::ShaderManager& shaderMgr = mResourceSystem->getSceneManager()->getShaderManager();
        osg::ref_ptr<osg::Program> program = shaderMgr.getProgram("water", defineMap);

        constexpr VFS::Path::NormalizedView waterImage("textures/omw/water_nm.png");
        osg::ref_ptr<osg::Texture2D> normalMap(
            new osg::Texture2D(mResourceSystem->getImageManager()->getImage(waterImage)));
        normalMap->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        normalMap->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
        mResourceSystem->getSceneManager()->applyFilterSettings(normalMap);

        mRainSettingsUpdater = new RainSettingsUpdater();
        node->setUpdateCallback(mRainSettingsUpdater);

        mShaderWaterStateSetUpdater = new ShaderWaterStateSetUpdater(
            this, mResourceSystem, mReflection, mRipples, std::move(program), std::move(normalMap));
        node->addCullCallback(mShaderWaterStateSetUpdater);
    }

    void Water::processChangedSettings(const Settings::CategorySettingVector& settings)
    {
        updateWaterMaterial();
    }

    Water::~Water()
    {
        mParent->removeChild(mWaterNode);

        if (mReflection)
        {
            mParent->removeChild(mReflection);
            mReflection = nullptr;
        }
        if (mRipples)
        {
            mParent->removeChild(mRipples);
            mRipples = nullptr;
            mSimulation->setRipples(nullptr);
        }
    }

    void Water::listAssetsToPreload(std::vector<VFS::Path::Normalized>& textures)
    {
        const int frameCount = std::clamp(Fallback::Map::getInt("Water_SurfaceFrameCount"), 0, 320);
        std::string_view texture = Fallback::Map::getString("Water_SurfaceTexture");
        for (int i = 0; i < frameCount; ++i)
        {
            std::ostringstream texname;
            texname << "textures/water/" << texture << std::setw(2) << std::setfill('0') << i << ".dds";
            textures.emplace_back(texname.str());
        }
    }

    void Water::setEnabled(bool enabled)
    {
        mEnabled = enabled;
        updateVisible();
    }

    void Water::changeCell(const MWWorld::CellStore* store)
    {
        bool isInterior = !store->getCell()->isExterior();
        bool wasInterior = mInterior;
        if (mWorldspace != store->getCell()->getWorldSpace())
        {
            mWorldspace = store->getCell()->getWorldSpace();
            mWaveDepthMapValid = false;
        }
        if (!isInterior)
        {
            mWaterNode->setPosition(
                getSceneNodeCoordinates(store->getCell()->getGridX(), store->getCell()->getGridY()));
            mInterior = false;
        }
        else
        {
            mWaterNode->setPosition(osg::Vec3f(0, 0, mTop));
            mInterior = true;
        }
        if (mInterior != wasInterior && mReflection)
            mReflection->setInterior(mInterior);
    }

    void Water::setHeight(const float height)
    {
        mTop = height;

        mSimulation->setWaterHeight(height);

        osg::Vec3f pos = mWaterNode->getPosition();
        pos.z() = height;
        mWaterNode->setPosition(pos);

        if (mReflection)
            mReflection->setWaterLevel(mTop);
    }

    void Water::setRainIntensity(float rainIntensity)
    {
        if (mRainSettingsUpdater)
            mRainSettingsUpdater->setRainIntensity(rainIntensity);
    }

    void Water::update(float dt, bool paused)
    {
        if (!paused)
        {
            mSimulation->update(dt);
        }

        if (mRipples)
        {
            mRipples->setPaused(paused);
        }
    }

    void Water::updateVisible()
    {
        bool visible = mEnabled && mToggled;
        mWaterNode->setNodeMask(visible ? ~0u : 0u);
        if (mReflection)
            mReflection->setNodeMask(visible ? Mask_RenderToTexture : 0u);
        if (mRipples)
            mRipples->setNodeMask(visible ? Mask_RenderToTexture : 0u);
    }

    bool Water::toggle()
    {
        mToggled = !mToggled;
        updateVisible();
        return mToggled;
    }

    bool Water::isUnderwater(const osg::Vec3f& pos) const
    {
        return pos.z() < mTop && mToggled && mEnabled;
    }

    osg::Vec3f Water::getSceneNodeCoordinates(int gridX, int gridY)
    {
        return osg::Vec3f(static_cast<float>(gridX * Constants::CellSizeInUnits + (Constants::CellSizeInUnits / 2)),
            static_cast<float>(gridY * Constants::CellSizeInUnits + (Constants::CellSizeInUnits / 2)), mTop);
    }

    void Water::addEmitter(const MWWorld::Ptr& ptr, float scale, float force)
    {
        mSimulation->addEmitter(ptr, scale, force);
    }

    void Water::removeEmitter(const MWWorld::Ptr& ptr)
    {
        mSimulation->removeEmitter(ptr);
    }

    void Water::updateEmitterPtr(const MWWorld::Ptr& old, const MWWorld::Ptr& ptr)
    {
        mSimulation->updateEmitterPtr(old, ptr);
    }

    void Water::emitRipple(const osg::Vec3f& pos)
    {
        mSimulation->emitRipple(pos);
    }

    void Water::removeCell(const MWWorld::CellStore* store)
    {
        mSimulation->removeCell(store);
    }

    void Water::clearRipples()
    {
        mSimulation->clear();
    }

    void Water::showWorld(bool show)
    {
        if (mReflection)
            mReflection->showWorld(show);
        mShowWorld = show;
    }

}
