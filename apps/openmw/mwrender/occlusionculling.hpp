#ifndef OPENMW_MWRENDER_OCCLUSIONCULLING_H
#define OPENMW_MWRENDER_OCCLUSIONCULLING_H

#include <osg/BoundingBox>
#include <osg/Camera>
#include <osg/Image>
#include <osg/Object>
#include <osg/Texture2D>
#include <osg/Vec3f>
#include <osg/observer_ptr>
#include <osg/ref_ptr>

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <components/sceneutil/nodecallback.hpp>

namespace MWRender
{
    struct OccluderMesh
    {
        osg::BoundingBox aabb;
        std::vector<osg::Vec3f> vertices; // world-space, shrunk toward centroid
        std::vector<unsigned int> indices; // triangle indices from actual geometry
    };

    /// Build a simplified occluder mesh from an OSG node's geometry.
    /// Collects world-space triangles, applies vertex clustering on a coarse 3D grid,
    /// and shrinks the result toward the centroid for conservative occlusion.
    /// @param node        Source node to collect geometry from
    /// @param gridRes     Grid resolution for vertex clustering (higher = more detail)
    /// @param shrinkFactor How much to shrink toward centroid (0..1, 1 = no shrink)
    /// @return Simplified mesh with AABB, or empty mesh if no geometry found
    OccluderMesh buildSimplifiedMesh(osg::Node* node, int gridRes, float shrinkFactor);

    /// Stored on paged chunk nodes (via SceneUtil::addUserData) to provide sub-object
    /// occluder meshes for MOC rasterization during cull traversal.
    struct PagedOccluders
    {
        std::vector<OccluderMesh> mOccluderMeshes;
    };

    /// Tag on object nodes that must never be occlusion-culled (doors: they sit flush
    /// against building surfaces and are easily hidden by the building's own occluder).
    struct SkipOcclusion
    {
    };
}

namespace SceneUtil
{
    class OcclusionCuller;
}

namespace MWRender
{
    class CellOcclusionCallback;

    /// Rasterization rules shared by every occluder path (the nearest-first pre-pass, cells and paged chunks).
    struct OccluderRules
    {
        float mMaxDistanceSq = 6144.f * 6144.f; ///< occluders farther than this (AABB centre) are not drawn
        float mInsideThreshold = 1.f; ///< skip an occluder whose AABB scaled by this contains the eye
        unsigned int mMaxTriangles = 30000; ///< per-frame building triangle budget, 0 = unlimited
    };

    /// Rasterize one occluder mesh if the rules allow it: within the distance, the eye not inside its (scaled) AABB
    /// (it would fill the whole buffer) and within the frame's triangle budget. Returns whether it was drawn.
    bool rasterizeOccluderMesh(SceneUtil::OcclusionCuller& culler, const OccluderMesh& mesh, const osg::Vec3f& eye,
        const OccluderRules& rules);

    /// OpenMGE XE: nearest-first occluder pre-pass. Occluders used to be drawn into the buffer in scene-graph order,
    /// interleaved with the visibility tests, so anything tested before a nearer building was drawn (in another cell,
    /// another paged chunk, or earlier in the same cell) could not be hidden by it. The occluder sources the main view
    /// drew last frame are remembered here; at the start of the next frame, right after the terrain, all their meshes
    /// are drawn nearest-first before any test runs. Sources that are new this frame still draw their occluders as they
    /// are reached, as before; already drawn meshes are skipped there.
    class OccluderRegistry : public osg::Referenced
    {
    public:
        /// The cell and chunk callbacks report themselves while the main view culls them.
        void noteCell(CellOcclusionCallback* callback);
        void noteChunk(osg::Node* chunk);

        /// Start of the main view's occlusion frame: draw last frame's sources nearest-first, then start collecting
        /// this frame's. Cells no longer attached under sceneRoot (unloaded since) are skipped, and so are cell
        /// objects that were not children of their cell last frame: a removed object never occludes.
        void rasterizeNearestFirst(SceneUtil::OcclusionCuller& culler, const osg::Vec3f& eye,
            const OccluderRules& rules, const osg::Node* sceneRoot);

        /// Whether the pre-pass already drew this mesh this frame.
        bool isRasterized(const OccluderMesh* mesh) const { return mRasterized.count(mesh) != 0; }

        /// Collected by CellOcclusionCallback::collectOccluders.
        struct Candidate
        {
            const OccluderMesh* mMesh;
            float mDistanceSq;
        };

    private:
        std::vector<osg::observer_ptr<CellOcclusionCallback>> mCells;
        std::vector<osg::observer_ptr<osg::Node>> mChunks;
        std::vector<Candidate> mCandidates; // scratch
        std::unordered_set<const OccluderMesh*> mRasterized;
        // the sources of the frame being culled, and the previous frame's
        std::vector<osg::observer_ptr<CellOcclusionCallback>> mNextCells;
        std::vector<osg::observer_ptr<osg::Node>> mNextChunks;
    };
}

namespace osgUtil
{
    class CullVisitor;
}

namespace osg
{
    class Group;
    class Node;
}

namespace SceneUtil
{
    class OcclusionCuller;
}

namespace Terrain
{
    class TerrainOccluder;
}

namespace MWRender
{
    /// Installed on the SceneRoot (LightManager). At the start of each main-camera cull,
    /// rasterizes terrain into the software occlusion buffer. Skips RTT cameras (shadows,
    /// water reflection) and interiors (no terrain data).
    class SceneOcclusionCallback
        : public SceneUtil::NodeCallback<SceneOcclusionCallback, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        SceneOcclusionCallback(SceneUtil::OcclusionCuller* culler, Terrain::TerrainOccluder* occluder, int radiusCells,
            bool enableTerrainOccluder, bool enableDebugOverlay, bool enableDebugMessages, bool enableInteriors,
            OccluderRegistry* registry, const OccluderRules& rules, bool enableStaticOccluders);

        void operator()(osg::Node* node, osgUtil::CullVisitor* cv);

        /// Update cell type flags. Call when the player transitions cells.
        void setCellType(bool isInterior, bool isQuasiExterior);

    private:
        void setupDebugOverlay();
        void updateDebugOverlay(osgUtil::CullVisitor* cv);

        osg::ref_ptr<SceneUtil::OcclusionCuller> mCuller;
        Terrain::TerrainOccluder* mTerrainOccluder;
        osg::ref_ptr<OccluderRegistry> mRegistry;
        OccluderRules mRules;
        bool mEnableStaticOccluders;
        int mRadiusCells;
        bool mEnableTerrainOccluder;
        bool mEnableDebugOverlay;
        bool mEnableDebugMessages;
        bool mEnableInteriors;
        bool mIsInterior = false;
        bool mIsQuasiExterior = false;
        unsigned int mLastFrameNumber = 0;

        // Scratch buffers reused across frames
        std::vector<osg::Vec3f> mPositions;
        std::vector<unsigned int> mIndices;

        // Debug overlay
        osg::ref_ptr<osg::Camera> mDebugCamera;
        osg::ref_ptr<osg::Image> mDebugImage;
        osg::ref_ptr<osg::Texture2D> mDebugTexture;
        std::vector<float> mDepthPixels;
    };

    /// Installed on paged chunk nodes (from ObjectPaging), distant and active-grid alike. Skips the chunk when its
    /// contents are fully hidden, otherwise rasterizes the chunk's pre-built occluder meshes (those the nearest-first
    /// pre-pass hasn't drawn yet) and traverses it.
    class PagedOccluderCallback
        : public SceneUtil::NodeCallback<PagedOccluderCallback, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        /// @param localBounds tight bounds of the chunk's contents in the chunk's local space (invalid: use the
        ///                    bounding sphere)
        PagedOccluderCallback(SceneUtil::OcclusionCuller* culler, OccluderRegistry* registry,
            const OccluderRules& rules, const osg::BoundingBox& localBounds);

        void operator()(osg::Node* node, osgUtil::CullVisitor* cv);

    private:
        osg::ref_ptr<SceneUtil::OcclusionCuller> mCuller;
        osg::ref_ptr<OccluderRegistry> mRegistry;
        OccluderRules mRules;
        osg::BoundingBox mLocalBounds;
    };

    /// Installed on each Cell Root group. Two-pass approach:
    /// Pass 1: Large objects (radius >= threshold) are tested against terrain depth, and if
    ///         visible, their shrunken AABB is rasterized as an occluder, then traversed.
    /// Pass 2: Small objects are tested against the enriched depth buffer (terrain + buildings).
    class CellOcclusionCallback
        : public SceneUtil::NodeCallback<CellOcclusionCallback, osg::Group*, osgUtil::CullVisitor*>
    {
    public:
        CellOcclusionCallback(SceneUtil::OcclusionCuller* culler, float occluderMinRadius, float occluderMaxRadius,
            float occluderShrinkFactor, int occluderMeshResolution, int occluderMaxMeshResolution,
            float occluderInsideThreshold, float occluderMaxDistance, bool enableStaticOccluders,
            unsigned int maxTriangles, OccluderRegistry* registry);

        void operator()(osg::Group* node, osgUtil::CullVisitor* cv);

        /// The cached occluder meshes of the cell's static occluders, for the nearest-first pre-pass.
        void collectOccluders(const osg::Vec3f& eye, std::vector<OccluderRegistry::Candidate>& out) const;

        /// The cell node this callback last culled.
        osg::Node* getCellNode() const { return mCellNode.get(); }

    private:
        struct CachedMesh
        {
            osg::observer_ptr<osg::Node> mNode;
            osg::BoundingSphere mBound;
            OccluderMesh mMesh;
            // Set once the node's bounds change (it moves or animates). A dynamic node is
            // never an occluder and is tested with its live bounds; its mesh is not rebuilt.
            bool mDynamic = false;
            // Last frame the node was a child of the cell; the pre-pass only uses current children.
            unsigned int mSeenFrame = ~0u;
        };

        /// Cached occluder mesh (actual triangles + AABB) for a node, built once per node. nullptr when the
        /// node has no mesh yet and this frame's build budget is spent (it is built on a later frame).
        CachedMesh* getOccluderEntry(osg::Node* node);

        osg::ref_ptr<SceneUtil::OcclusionCuller> mCuller;
        float mOccluderMinRadius;
        float mOccluderMaxRadius;
        float mOccluderShrinkFactor;
        int mOccluderMeshResolution;
        int mOccluderMaxMeshResolution;
        float mOccluderInsideThreshold;
        float mOccluderMaxDistanceSq;
        bool mEnableStaticOccluders;
        unsigned int mMaxTriangles;
        osg::ref_ptr<OccluderRegistry> mRegistry;
        unsigned int mLastFrame = ~0u - 1; // frame of the most recent main-view cull of this cell
        osg::observer_ptr<osg::Node> mCellNode;

        std::unordered_map<osg::Node*, CachedMesh> mMeshCache;
        std::vector<char> mHandledInFirstPass; // scratch, reused across frames
    };
}

#endif
