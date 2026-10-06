#include "occlusionculling.hpp"

#include "vismask.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <queue>
#include <unordered_set>

#include <osg/AlphaFunc>
#include <osg/BoundingBox>
#include <osg/BoundingSphere>
#include <osg/Camera>
#include <osg/ComputeBoundsVisitor>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/NodeVisitor>
#include <osg/Transform>
#include <osgUtil/CullVisitor>

#include <components/debug/debuglog.hpp>
#include <components/misc/constants.hpp>
#include <components/sceneutil/cullprofile.hpp>
#include <components/sceneutil/occlusionculling.hpp>
#include <components/sceneutil/userdata.hpp>
#include <components/terrain/terrainoccluder.hpp>

namespace
{
    class CollectMeshVisitor : public osg::NodeVisitor
    {
    public:
        CollectMeshVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Transform& transform) override
        {
            osg::Matrix matrix;
            if (!mMatrixStack.empty())
                matrix = mMatrixStack.back();
            transform.computeLocalToWorldMatrix(matrix, this);
            mMatrixStack.push_back(matrix);
            traverse(transform);
            mMatrixStack.pop_back();
        }

        void apply(osg::Drawable& drawable) override
        {
            auto* geom = drawable.asGeometry();
            if (!geom)
                return;

            // Skip alpha-tested drawables (e.g. tree leaf billboards),
            // their transparent regions would cause false occlusion.
            // NIF loader applies AlphaFunc to the parent Group, not the Geometry itself,
            // so check both the drawable and its immediate parent.
            if (hasAlphaTest(drawable))
                return;

            const auto* verts = dynamic_cast<const osg::Vec3Array*>(geom->getVertexArray());
            if (!verts || verts->empty())
                return;

            osg::Matrix matrix;
            if (!mMatrixStack.empty())
                matrix = mMatrixStack.back();

            unsigned int baseVertex = static_cast<unsigned int>(mVertices.size());
            for (const auto& v : *verts)
                mVertices.push_back(v * matrix);

            for (unsigned int p = 0; p < geom->getNumPrimitiveSets(); ++p)
                collectTriangles(geom->getPrimitiveSet(p), baseVertex);
        }

        std::vector<osg::Vec3f> mVertices;
        std::vector<unsigned int> mIndices;

    private:
        static bool hasAlphaTest(const osg::Node& node)
        {
            if (const auto* ss = node.getStateSet())
                if (ss->getAttribute(osg::StateAttribute::ALPHAFUNC))
                    return true;
            for (const auto* parent : node.getParents())
                if (const auto* ss = parent->getStateSet())
                    if (ss->getAttribute(osg::StateAttribute::ALPHAFUNC))
                        return true;
            return false;
        }

        void collectTriangles(const osg::PrimitiveSet* pset, unsigned int baseVertex)
        {
            unsigned int count = pset->getNumIndices();
            switch (pset->getMode())
            {
                case GL_TRIANGLES:
                    for (unsigned int i = 0; i + 2 < count; i += 3)
                    {
                        mIndices.push_back(baseVertex + pset->index(i));
                        mIndices.push_back(baseVertex + pset->index(i + 1));
                        mIndices.push_back(baseVertex + pset->index(i + 2));
                    }
                    break;
                case GL_TRIANGLE_STRIP:
                    for (unsigned int i = 0; i + 2 < count; ++i)
                    {
                        if (i % 2 == 0)
                        {
                            mIndices.push_back(baseVertex + pset->index(i));
                            mIndices.push_back(baseVertex + pset->index(i + 1));
                            mIndices.push_back(baseVertex + pset->index(i + 2));
                        }
                        else
                        {
                            mIndices.push_back(baseVertex + pset->index(i + 1));
                            mIndices.push_back(baseVertex + pset->index(i));
                            mIndices.push_back(baseVertex + pset->index(i + 2));
                        }
                    }
                    break;
                case GL_TRIANGLE_FAN:
                    for (unsigned int i = 1; i + 1 < count; ++i)
                    {
                        mIndices.push_back(baseVertex + pset->index(0));
                        mIndices.push_back(baseVertex + pset->index(i));
                        mIndices.push_back(baseVertex + pset->index(i + 1));
                    }
                    break;
                default:
                    break;
            }
        }

        std::vector<osg::Matrix> mMatrixStack;
    };
}

namespace MWRender
{
    OccluderMesh buildSimplifiedMesh(osg::Node* node, int gridRes, float shrinkFactor)
    {
        OccluderMesh mesh;

        CollectMeshVisitor cmv;
        node->accept(cmv);

        if (cmv.mIndices.empty() || cmv.mVertices.size() < 3)
        {
            osg::ComputeBoundsVisitor cbv;
            node->accept(cbv);
            mesh.aabb = cbv.getBoundingBox();
            return mesh;
        }

        for (const auto& v : cmv.mVertices)
            mesh.aabb.expandBy(v);
        const unsigned int res = static_cast<unsigned int>(gridRes);
        float dx = mesh.aabb.xMax() - mesh.aabb.xMin();
        float dy = mesh.aabb.yMax() - mesh.aabb.yMin();
        float dz = mesh.aabb.zMax() - mesh.aabb.zMin();
        float maxDim = std::max({ dx, dy, dz });
        float cellSize = maxDim / res;

        if (cellSize > 0)
        {
            unsigned int resX = std::max(1u, static_cast<unsigned int>(std::ceil(dx / cellSize)));
            unsigned int resY = std::max(1u, static_cast<unsigned int>(std::ceil(dy / cellSize)));

            struct CellData
            {
                osg::Vec3f sum;
                unsigned int count = 0;
                unsigned int newIndex = 0;
            };
            std::unordered_map<unsigned int, CellData> cells;
            std::vector<unsigned int> vertexRemap(cmv.mVertices.size());

            for (size_t i = 0; i < cmv.mVertices.size(); ++i)
            {
                const osg::Vec3f& v = cmv.mVertices[i];
                float fx = (v.x() - mesh.aabb.xMin()) / cellSize;
                float fy = (v.y() - mesh.aabb.yMin()) / cellSize;
                float fz = (v.z() - mesh.aabb.zMin()) / cellSize;
                unsigned int gx = std::min(static_cast<unsigned int>(std::max(fx, 0.0f)), resX - 1);
                unsigned int gy = std::min(static_cast<unsigned int>(std::max(fy, 0.0f)), resY - 1);
                unsigned int gz = std::min(static_cast<unsigned int>(std::max(fz, 0.0f)), res - 1);
                unsigned int cellId = gx + gy * resX + gz * resX * resY;

                auto& cell = cells[cellId];
                cell.sum += v;
                cell.count++;
                vertexRemap[i] = cellId;
            }

            unsigned int nextIdx = 0;
            for (auto& [id, cell] : cells)
            {
                cell.newIndex = nextIdx++;
                mesh.vertices.push_back(cell.sum / static_cast<float>(cell.count));
            }

            std::unordered_set<uint64_t> seen;
            for (size_t i = 0; i + 2 < cmv.mIndices.size(); i += 3)
            {
                unsigned int a = cells[vertexRemap[cmv.mIndices[i]]].newIndex;
                unsigned int b = cells[vertexRemap[cmv.mIndices[i + 1]]].newIndex;
                unsigned int c = cells[vertexRemap[cmv.mIndices[i + 2]]].newIndex;

                // Reject degenerate triangles
                if (a == b || b == c || a == c)
                    continue;

                // Canonical form: rotate min index to front, then ensure consistent order
                unsigned int tri[3] = { a, b, c };
                if (tri[1] < tri[0] && tri[1] < tri[2])
                    std::rotate(tri, tri + 1, tri + 3);
                else if (tri[2] < tri[0] && tri[2] < tri[1])
                    std::rotate(tri, tri + 2, tri + 3);
                if (tri[1] > tri[2])
                    std::swap(tri[1], tri[2]);

                uint64_t key = (uint64_t(tri[0]) << 42) | (uint64_t(tri[1]) << 21) | uint64_t(tri[2]);
                if (seen.insert(key).second)
                {
                    mesh.indices.push_back(a);
                    mesh.indices.push_back(b);
                    mesh.indices.push_back(c);
                }
            }
        }

        if (!mesh.vertices.empty())
        {
            osg::Vec3f center(0, 0, 0);
            for (const auto& v : mesh.vertices)
                center += v;
            center /= static_cast<float>(mesh.vertices.size());
            for (auto& v : mesh.vertices)
                v = center + (v - center) * shrinkFactor;
        }

        return mesh;
    }

    bool rasterizeOccluderMesh(
        SceneUtil::OcclusionCuller& culler, const OccluderMesh& mesh, const osg::Vec3f& eye, const OccluderRules& rules)
    {
        if (mesh.indices.empty() || !mesh.aabb.valid())
            return false;

        // Distant buildings cover few pixels; terrain handles far-distance occlusion.
        const osg::Vec3f center = mesh.aabb.center();
        if ((center - eye).length2() > rules.mMaxDistanceSq)
            return false;

        // Don't rasterize when the eye is inside the (scaled) AABB: it would fill the entire buffer.
        const osg::Vec3f halfExtent
            = (osg::Vec3f(mesh.aabb.xMax(), mesh.aabb.yMax(), mesh.aabb.zMax()) - center) * rules.mInsideThreshold;
        osg::BoundingBox scaledBB;
        scaledBB.expandBy(center - halfExtent);
        scaledBB.expandBy(center + halfExtent);
        if (scaledBB.contains(eye))
            return false;

        const unsigned int newTris = static_cast<unsigned int>(mesh.indices.size() / 3);
        if (rules.mMaxTriangles > 0 && culler.getNumBuildingTris() + newTris > rules.mMaxTriangles)
            return false;

        culler.rasterizeOccluder(mesh.vertices, mesh.indices);
        culler.incrementBuildingOccluders(newTris, static_cast<unsigned int>(mesh.vertices.size()));
        return true;
    }

    void OccluderRegistry::noteCell(CellOcclusionCallback* callback)
    {
        mNextCells.emplace_back(callback);
    }

    void OccluderRegistry::noteChunk(osg::Node* chunk)
    {
        mNextChunks.emplace_back(chunk);
    }

    namespace
    {
        /// Whether node is (still) in the scene graph below root.
        bool isUnder(const osg::Node* node, const osg::Node* root, int depth = 0)
        {
            if (node == root)
                return true;
            if (depth > 32)
                return false;
            for (const osg::Group* parent : node->getParents())
                if (isUnder(parent, root, depth + 1))
                    return true;
            return false;
        }
    }

    void OccluderRegistry::rasterizeNearestFirst(SceneUtil::OcclusionCuller& culler, const osg::Vec3f& eye,
        const OccluderRules& rules, const osg::Node* sceneRoot)
    {
        // Last frame's sources become this frame's pre-pass input; this frame's start empty.
        mCells.swap(mNextCells);
        mChunks.swap(mNextChunks);
        mNextCells.clear();
        mNextChunks.clear();
        mRasterized.clear();
        mCandidates.clear();

        for (const auto& weakCell : mCells)
        {
            osg::ref_ptr<CellOcclusionCallback> cell;
            if (weakCell.lock(cell) && cell->getCellNode() != nullptr && isUnder(cell->getCellNode(), sceneRoot))
                cell->collectOccluders(eye, mCandidates);
        }
        for (const auto& weakChunk : mChunks)
        {
            // Paged chunks have no scene-graph parent (the terrain's QuadTreeWorld traverses their nodes by hand),
            // so "drawn last frame" is the test. Their contents only change when the paging rebuilds them (a paged
            // static disabled by a script), which the next view picks up anyway.
            osg::ref_ptr<osg::Node> chunk;
            if (!weakChunk.lock(chunk))
                continue;
            // Chunk occluder meshes are stored in world space.
            if (const PagedOccluders* pod = SceneUtil::findUserData<PagedOccluders>(*chunk))
                for (const OccluderMesh& mesh : pod->mOccluderMeshes)
                    if (!mesh.indices.empty() && mesh.aabb.valid())
                        mCandidates.push_back({ &mesh, (mesh.aabb.center() - eye).length2() });
        }

        std::sort(mCandidates.begin(), mCandidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.mDistanceSq < b.mDistanceSq; });

        for (const Candidate& candidate : mCandidates)
        {
            if (candidate.mDistanceSq > rules.mMaxDistanceSq)
                break; // sorted: everything after is farther
            // A mesh shared by two sources (e.g. the same chunk listed twice) is drawn once.
            if (mRasterized.count(candidate.mMesh) != 0)
                continue;
            if (rasterizeOccluderMesh(culler, *candidate.mMesh, eye, rules))
                mRasterized.insert(candidate.mMesh);
        }
    }

    SceneOcclusionCallback::SceneOcclusionCallback(SceneUtil::OcclusionCuller* culler,
        Terrain::TerrainOccluder* occluder, int radiusCells, bool enableTerrainOccluder, bool enableDebugOverlay,
        bool enableDebugMessages, bool enableInteriors, OccluderRegistry* registry, const OccluderRules& rules,
        bool enableStaticOccluders)
        : mCuller(culler)
        , mTerrainOccluder(occluder)
        , mRegistry(registry)
        , mRules(rules)
        , mEnableStaticOccluders(enableStaticOccluders)
        , mRadiusCells(radiusCells)
        , mEnableTerrainOccluder(enableTerrainOccluder)
        , mEnableDebugOverlay(enableDebugOverlay)
        , mEnableDebugMessages(enableDebugMessages)
        , mEnableInteriors(enableInteriors)
    {
    }

    void SceneOcclusionCallback::setCellType(bool isInterior, bool isQuasiExterior)
    {
        mIsInterior = isInterior;
        mIsQuasiExterior = isQuasiExterior;
    }

    void SceneOcclusionCallback::setupDebugOverlay()
    {
        unsigned int w, h;
        mCuller->getResolution(w, h);
        if (w == 0 || h == 0)
            return;

        mDepthPixels.resize(w * h);

        // Create image to hold depth data (luminance float -> converted to RGBA)
        mDebugImage = new osg::Image;
        mDebugImage->allocateImage(w, h, 1, GL_LUMINANCE, GL_FLOAT);

        // Create texture from image
        mDebugTexture = new osg::Texture2D(mDebugImage);
        mDebugTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        mDebugTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
        mDebugTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        mDebugTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        mDebugTexture->setResizeNonPowerOfTwoHint(false);

        // Create POST_RENDER camera in corner of screen
        mDebugCamera = new osg::Camera;
        mDebugCamera->setName("OcclusionDebugCamera");
        mDebugCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        mDebugCamera->setRenderOrder(osg::Camera::POST_RENDER, 100);
        mDebugCamera->setAllowEventFocus(false);
        mDebugCamera->setClearMask(0);
        mDebugCamera->setProjectionMatrix(osg::Matrix::ortho2D(0, 1, 0, 1));
        mDebugCamera->setViewMatrix(osg::Matrix::identity());
        mDebugCamera->getOrCreateStateSet()->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        mDebugCamera->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
        mDebugCamera->setCullingActive(false);

        // Scale viewport to show in bottom-left corner (400px wide, aspect-correct height)
        float displayWidth = 400.0f;
        float displayHeight = displayWidth * static_cast<float>(h) / static_cast<float>(w);
        mDebugCamera->setViewport(0, 0, static_cast<int>(displayWidth), static_cast<int>(displayHeight));

        // Create textured quad
        osg::ref_ptr<osg::Geometry> quad
            = osg::createTexturedQuadGeometry(osg::Vec3(0, 0, 0), osg::Vec3(1, 0, 0), osg::Vec3(0, 1, 0));
        quad->setCullingActive(false);

        osg::StateSet* ss = quad->getOrCreateStateSet();
        ss->setTextureAttributeAndModes(0, mDebugTexture, osg::StateAttribute::ON);

        mDebugCamera->addChild(quad);
    }

    void SceneOcclusionCallback::updateDebugOverlay(osgUtil::CullVisitor* cv)
    {
        if (!mDebugCamera)
            return;

        unsigned int w, h;
        mCuller->getResolution(w, h);

        // Read depth buffer from MOC
        mCuller->computePixelDepthBuffer(mDepthPixels.data());

        // Copy to image (normalize: MOC stores 1/w, so closer = larger values)
        float* imageData = reinterpret_cast<float*>(mDebugImage->data());
        for (unsigned int i = 0; i < w * h; ++i)
        {
            float d = mDepthPixels[i];
            // MOC depth is 1/w (reciprocal clip-space w). 0 = far/empty, larger = closer.
            // Clamp and invert for visualization: dark = far, bright = near
            imageData[i] = std::min(d * 50.0f, 1.0f);
        }
        mDebugImage->dirty();

        // Inject debug camera into the cull visitor so it gets rendered
        unsigned int traversalMask = cv->getTraversalMask();
        cv->setTraversalMask(0xffffffff);
        mDebugCamera->accept(*cv);
        cv->setTraversalMask(traversalMask);
    }

    void SceneOcclusionCallback::operator()(osg::Node* node, osgUtil::CullVisitor* cv)
    {
        // Only run occlusion for the main scene camera.
        // Skip shadow cameras, water reflection, and any other cameras.
        osg::Camera* cam = cv->getCurrentCamera();
        if (cam->getName() != Constants::SceneCamera)
        {
            traverse(node, cv);
            return;
        }

        // The scene is traversed multiple times per frame: once for the main cull pass,
        // and again by MWShadowTechnique::cullShadowReceivingScene (same camera name).
        // Only set up MOC on the first traversal; subsequent passes just traverse normally.
        unsigned int frameNumber = cv->getFrameStamp()->getFrameNumber();
        if (frameNumber == mLastFrameNumber)
        {
            traverse(node, cv);
            return;
        }
        mLastFrameNumber = frameNumber;

        // Skip MSOC entirely in interiors (unless enabled via setting)
        if (mIsInterior && !mEnableInteriors)
        {
            traverse(node, cv);
            return;
        }

        // Begin occlusion frame with camera matrices
        std::optional<SceneUtil::CullProfile::Scope> profile(std::in_place, SceneUtil::CullProfile::Section::Occluders);
        mCuller->beginFrame(cam->getViewMatrix(), cam->getProjectionMatrix());

        // Build and rasterize terrain occluder mesh (skip for quasi-exteriors and interiors, no real terrain)
        if (mEnableTerrainOccluder && !mIsQuasiExterior && !mIsInterior && mTerrainOccluder->hasTerrainData())
        {
            mPositions.clear();
            mIndices.clear();
            mTerrainOccluder->build(cv->getEyePoint(), mRadiusCells, mPositions, mIndices);

            if (!mPositions.empty())
                mCuller->rasterizeOccluder(mPositions, mIndices);
        }

        // Buildings the main view drew last frame, nearest first, before any visibility test.
        if (mRegistry && mEnableStaticOccluders)
            mRegistry->rasterizeNearestFirst(*mCuller, cv->getEyePoint(), mRules, node);

        profile.reset();

        // Continue normal cull traversal, CellOcclusionCallbacks will test against the buffer
        traverse(node, cv);

        // End the occlusion frame so sub-camera traversals (water reflection/refraction,
        // shadow cameras) that share this scene graph don't incorrectly cull against
        // the main camera's occlusion buffer.
        mCuller->endFrame();

        // Update debug overlay after traversal (terrain + building occluders now in buffer)
        if (mEnableDebugOverlay)
        {
            if (!mDebugCamera)
                setupDebugOverlay();
            updateDebugOverlay(cv);
        }

        if (mEnableDebugMessages)
        {
            static int frameCount = 0;
            if (++frameCount % 300 == 0)
            {
                const auto terrainTris = mIndices.size() / 3;
                const auto bldgTris = mCuller->getNumBuildingTris();
                const auto terrainVerts = mPositions.size();
                const auto bldgVerts = mCuller->getNumBuildingVerts();
                Log(Debug::Info) << "OcclusionCull: terrain tris=" << terrainTris << " terrain verts=" << terrainVerts
                                 << " bldg occluders=" << mCuller->getNumBuildingOccluders()
                                 << " bldg tris=" << bldgTris << " bldg verts=" << bldgVerts
                                 << " total tris=" << (terrainTris + bldgTris)
                                 << " total verts=" << (terrainVerts + bldgVerts)
                                 << " tested=" << mCuller->getNumTested() << " occluded=" << mCuller->getNumOccluded();
            }
        }
    }

    PagedOccluderCallback::PagedOccluderCallback(SceneUtil::OcclusionCuller* culler, OccluderRegistry* registry,
        const OccluderRules& rules, const osg::BoundingBox& localBounds)
        : mCuller(culler)
        , mRegistry(registry)
        , mRules(rules)
        , mLocalBounds(localBounds)
    {
    }

    void PagedOccluderCallback::operator()(osg::Node* node, osgUtil::CullVisitor* cv)
    {
        if (!mCuller->isFrameActive())
        {
            traverse(node, cv);
            return;
        }

        // The chunk sits under a PAT, so its bounds are in chunk-local space.
        osg::Matrixd viewInverse;
        viewInverse.invert(cv->getCurrentCamera()->getViewMatrix());
        const osg::Matrixd modelToWorld = *cv->getModelViewMatrix() * viewInverse;

        osg::BoundingBox worldBB;
        if (mLocalBounds.valid())
        {
            // Tight bounds of the chunk's contents: much smaller than the box around the bounding sphere for the
            // flat, wide chunks of a town, so far more of them can be found fully hidden.
            for (unsigned int i = 0; i < 8; ++i)
                worldBB.expandBy(mLocalBounds.corner(i) * modelToWorld);
        }
        else
        {
            const osg::BoundingSphere& bs = node->getBound();
            if (bs.valid())
            {
                const osg::Vec3f worldCenter = bs.center() * modelToWorld;
                const float r = bs.radius();
                worldBB = osg::BoundingBox(worldCenter - osg::Vec3f(r, r, r), worldCenter + osg::Vec3f(r, r, r));
            }
        }

        if (worldBB.valid())
        {
            // Entire chunk hidden: skip rasterization and traversal. (With the eye inside the box the test always
            // passes, so the chunk around the camera is never skipped.)
            if (!mCuller->testVisibleAABB(worldBB))
                return;

            if (const PagedOccluders* pod = SceneUtil::findUserData<PagedOccluders>(*node))
            {
                const osg::Vec3f eyeWorld(viewInverse(3, 0), viewInverse(3, 1), viewInverse(3, 2));
                for (const auto& occMesh : pod->mOccluderMeshes)
                {
                    // already drawn by the nearest-first pre-pass this frame
                    if (mRegistry && mRegistry->isRasterized(&occMesh))
                        continue;
                    rasterizeOccluderMesh(*mCuller, occMesh, eyeWorld, mRules);
                }
                if (mRegistry)
                    mRegistry->noteChunk(node);
            }
        }

        traverse(node, cv);
    }

    CellOcclusionCallback::CellOcclusionCallback(SceneUtil::OcclusionCuller* culler, float occluderMinRadius,
        float occluderMaxRadius, float occluderShrinkFactor, int occluderMeshResolution, int occluderMaxMeshResolution,
        float occluderInsideThreshold, float occluderMaxDistance, bool enableStaticOccluders, unsigned int maxTriangles,
        OccluderRegistry* registry)
        : mCuller(culler)
        , mOccluderMinRadius(occluderMinRadius)
        , mOccluderMaxRadius(occluderMaxRadius)
        , mOccluderShrinkFactor(occluderShrinkFactor)
        , mOccluderMeshResolution(occluderMeshResolution)
        , mOccluderMaxMeshResolution(occluderMaxMeshResolution)
        , mOccluderInsideThreshold(occluderInsideThreshold)
        , mOccluderMaxDistanceSq(occluderMaxDistance * occluderMaxDistance)
        , mEnableStaticOccluders(enableStaticOccluders)
        , mMaxTriangles(maxTriangles)
        , mRegistry(registry)
    {
    }

    void CellOcclusionCallback::collectOccluders(
        const osg::Vec3f& eye, std::vector<OccluderRegistry::Candidate>& out) const
    {
        if (!mEnableStaticOccluders)
            return;
        for (const auto& [node, entry] : mMeshCache)
        {
            // Only objects that were occluders in the cell's last cull and are still its children now: an object
            // removed since (a scripted Disable, a deletion) must never occlude. Moving/animated objects never do.
            if (entry.mSeenFrame != mLastFrame || entry.mDynamic || entry.mMesh.indices.empty()
                || !entry.mMesh.aabb.valid())
                continue;
            osg::ref_ptr<osg::Node> object;
            if (!entry.mNode.lock(object) || object.get() != node)
                continue;
            const osg::Node::ParentList& parents = object->getParents();
            if (std::find(parents.begin(), parents.end(), mCellNode.get()) == parents.end())
                continue;
            out.push_back({ &entry.mMesh, (entry.mMesh.aabb.center() - eye).length2() });
        }
    }

    CellOcclusionCallback::CachedMesh* CellOcclusionCallback::getOccluderEntry(osg::Node* node)
    {
        // The mesh is built in world space once per node. If the node later moves or animates
        // (its bounds change), it is marked dynamic instead of rebuilt every frame: from then on
        // it is never an occluder and is tested with its live bounds. A deleted node's address
        // can be reused by a new one, so the entry also checks it still refers to the same node.
        const osg::BoundingSphere& currentBound = node->getBound();
        auto it = mMeshCache.find(node);
        if (it != mMeshCache.end())
        {
            osg::ref_ptr<osg::Node> alive;
            if (it->second.mNode.lock(alive) && alive.get() == node)
            {
                CachedMesh& cached = it->second;
                if (!cached.mDynamic
                    && ((cached.mBound.center() - currentBound.center()).length2() >= 1.f
                        || std::abs(cached.mBound.radius() - currentBound.radius()) >= 1.f))
                    cached.mDynamic = true;
                return &cached;
            }
            mMeshCache.erase(it);
        }

        if (!mCuller->canBuildOccluderMesh())
            return nullptr;
        const auto buildStart = std::chrono::steady_clock::now();

        int meshRes = mOccluderMeshResolution;
        const float radius = currentBound.radius();
        if (radius > mOccluderMinRadius && mOccluderMinRadius > 0)
        {
            const float scale = radius / mOccluderMinRadius;
            meshRes = std::clamp(
                static_cast<int>(mOccluderMeshResolution * scale), mOccluderMeshResolution, mOccluderMaxMeshResolution);
        }

        CachedMesh entry;
        entry.mNode = node;
        entry.mBound = currentBound;
        entry.mMesh = buildSimplifiedMesh(node, meshRes, mOccluderShrinkFactor);
        mCuller->addOccluderMeshBuild(
            std::chrono::duration<double>(std::chrono::steady_clock::now() - buildStart).count());
        return &mMeshCache.emplace(node, std::move(entry)).first->second;
    }

    void CellOcclusionCallback::operator()(osg::Group* node, osgUtil::CullVisitor* cv)
    {
        // If occlusion is not active this frame (interior, shadow camera, etc.), traverse normally
        if (!mCuller->isFrameActive())
        {
            traverse(node, cv);
            return;
        }

        // Test cell bounding box first, if fully occluded, skip entire cell
        const osg::BoundingSphere& cellBS = node->getBound();
        if (cellBS.valid())
        {
            osg::BoundingBox cellBB;
            cellBB.expandBy(cellBS);

            if (!mCuller->testVisibleAABB(cellBB))
                return; // Entire cell occluded, no children traversed
        }

        mLastFrame = cv->getFrameStamp()->getFrameNumber();
        mCellNode = node;
        if (mRegistry)
            mRegistry->noteCell(this);
        const OccluderRules rules{ mOccluderMaxDistanceSq, mOccluderInsideThreshold, mMaxTriangles };

        const unsigned int numChildren = node->getNumChildren();
        mHandledInFirstPass.assign(numChildren, 0);

        // Pass 1: Large static objects, test against terrain depth, optionally rasterize as occluders.
        // Everything else (small, moving or animated objects, actors) is left for pass 2.
        for (unsigned int i = 0; i < numChildren; ++i)
        {
            osg::Node* child = node->getChild(i);
            const osg::BoundingSphere& bs = child->getBound();

            if (!bs.valid() || bs.radius() < mOccluderMinRadius)
                continue;

            // Actors move and animate every frame: never occluders, tested with live bounds.
            if ((child->getNodeMask() & (Mask_Actor | Mask_Player)) != 0)
                continue;

            // Paged chunks and other oversized objects, test visibility, rasterize stored occluders
            if (bs.radius() > mOccluderMaxRadius)
            {
                // Rasterize sub-object occluder meshes stored at chunk creation time
                if (mEnableStaticOccluders)
                {
                    if (const PagedOccluders* pod = SceneUtil::findUserData<PagedOccluders>(*child))
                    {
                        for (const auto& occMesh : pod->mOccluderMeshes)
                        {
                            if (mRegistry && mRegistry->isRasterized(&occMesh))
                                continue;
                            rasterizeOccluderMesh(*mCuller, occMesh, cv->getEyePoint(), rules);
                        }
                    }
                }

                // Test chunk visibility against depth buffer (may now include its own occluders)
                osg::BoundingBox pageBB;
                pageBB.expandBy(bs);
                if (mCuller->testVisibleAABB(pageBB))
                    child->accept(*cv);
                mHandledInFirstPass[i] = 1;
                continue;
            }

            // Cached occluder mesh (with AABB for the visibility test). Dynamic objects, objects
            // without usable geometry bounds and objects whose mesh isn't built yet (this frame's
            // build budget is spent) go to pass 2 (never skipped untested).
            CachedMesh* entry = getOccluderEntry(child);
            if (entry == nullptr || entry->mDynamic || !entry->mMesh.aabb.valid())
                continue;
            entry->mSeenFrame = mLastFrame; // a current child: eligible for next frame's pre-pass
            const OccluderMesh& mesh = entry->mMesh;
            mHandledInFirstPass[i] = 1;

            if (mCuller->testVisibleAABB(mesh.aabb))
            {
                // Rasterize as an occluder unless the nearest-first pre-pass already did
                if (mEnableStaticOccluders && !(mRegistry && mRegistry->isRasterized(&mesh)))
                    rasterizeOccluderMesh(*mCuller, mesh, cv->getEyePoint(), rules);

                child->accept(*cv);
            }
            // else: occluded by terrain, skip entirely
        }

        // Pass 2: everything pass 1 did not handle (small objects, actors, moving or animated
        // objects), tested with live bounds against the enriched depth buffer (terrain + buildings)
        for (unsigned int i = 0; i < numChildren; ++i)
        {
            if (mHandledInFirstPass[i])
                continue;

            osg::Node* child = node->getChild(i);
            const osg::BoundingSphere& bs = child->getBound();

            if (!bs.valid())
            {
                child->accept(*cv);
                continue;
            }

            // Never occlude doors, they sit flush against building surfaces
            // and are easily falsely hidden by the parent building's AABB occluder
            const bool skipOcclusion = SceneUtil::findUserData<SkipOcclusion>(*child) != nullptr;

            osg::BoundingBox childBB;
            childBB.expandBy(bs);

            if (skipOcclusion || mCuller->testVisibleAABB(childBB))
                child->accept(*cv);
            // else: occluded, skip
        }
    }
}
