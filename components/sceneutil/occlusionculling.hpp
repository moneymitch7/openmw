#ifndef OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONCULLING_H
#define OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONCULLING_H

#include <osg/BoundingBox>
#include <osg/Matrixd>
#include <osg/Referenced>
#include <osg/Vec3f>

#include <vector>

class MaskedOcclusionCulling;

namespace SceneUtil
{
    /// Wraps Intel's Masked Software Occlusion Culling library.
    /// Provides a CPU-based hierarchical depth buffer for occlusion testing during cull traversal.
    class OcclusionCuller : public osg::Referenced
    {
    public:
        OcclusionCuller(unsigned int bufferWidth, unsigned int bufferHeight);
        ~OcclusionCuller();

        /// Call at the start of each frame's cull traversal.
        /// Clears the depth buffer and stores the view-projection matrix.
        /// OpenMGE XE: resetStats false keeps the frame's counters (a second view in the same frame, the water
        /// reflection, adds to the main view's instead of replacing them).
        void beginFrame(
            const osg::Matrixd& viewMatrix, const osg::Matrixd& projectionMatrix, bool resetStats = true);

        /// OpenMGE XE: the view this frame is culled for (world to eye, the scene root's model-view) and the node
        /// mask it draws: occluders are measured from its eye (distance, eye inside an occluder), and only what the
        /// view draws may occlude.
        void setFrameView(const osg::Matrixd& view, unsigned int occluderMask)
        {
            mFrameViewInverse.invert(view);
            mFrameEye = mFrameViewInverse.getTrans();
            mOccluderMask = occluderMask;
        }
        const osg::Vec3f& getFrameEye() const { return mFrameEye; }
        const osg::Matrixd& getFrameViewInverse() const { return mFrameViewInverse; }
        unsigned int getOccluderMask() const { return mOccluderMask; }

        /// OpenMGE XE: until endFrame, occluders only count above this height (world z): the water reflection
        /// clips away everything below the water, which must not hide anything there.
        void setOccluderClipBelow(float z)
        {
            mClipBelow = true;
            mClipZ = z;
        }

        /// Rasterize world-space triangles as occluders into the depth buffer.
        void rasterizeOccluder(const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices);

        /// Rasterize a world-space AABB as an occluder (12 triangles for 6 faces).
        void rasterizeAABBOccluder(const osg::BoundingBox& worldBB);

        /// Test if a world-space AABB is visible (not fully occluded).
        /// Returns true if the box may be visible, false if definitely occluded.
        bool testVisibleAABB(const osg::BoundingBox& worldBB) const;

        /// Safety margin in occlusion-buffer pixels: the tested screen rect
        /// is inflated by this amount on every side, so an object is culled
        /// only when occluded with room to spare (0 = exact test).
        void setTestMargin(float bufferPixels) { mTestMarginPx = bufferPixels; }

        bool isActive() const { return mMOC != nullptr; }
        bool isFrameActive() const { return mFrameActive; }
        void endFrame()
        {
            mFrameActive = false;
            mClipBelow = false;
        }

        unsigned int getNumOccluded() const { return mNumOccluded; }
        unsigned int getNumTested() const { return mNumTested; }
        unsigned int getNumBuildingOccluders() const { return mNumBuildingOccluders; }
        unsigned int getNumBuildingTris() const { return mNumBuildingTris; }
        unsigned int getNumBuildingVerts() const { return mNumBuildingVerts; }
        void incrementBuildingOccluders(unsigned int tris, unsigned int verts)
        {
            ++mNumBuildingOccluders;
            mNumBuildingTris += tris;
            mNumBuildingVerts += verts;
        }

        /// Per-frame time budget for building new occluder meshes, shared by every cell. A town coming into
        /// view would otherwise build all of its meshes in one frame; objects over the budget wait for a later
        /// frame (they are drawn and tested with their bounds meanwhile). One build always goes through.
        bool canBuildOccluderMesh() const { return mMeshBuildsThisFrame == 0 || mMeshBuildSeconds < sMeshBuildBudget; }
        void addOccluderMeshBuild(double seconds)
        {
            ++mMeshBuildsThisFrame;
            mMeshBuildSeconds += seconds;
        }

        /// Write the per-pixel depth buffer to depthData (width*height floats, bottom-to-top).
        void computePixelDepthBuffer(float* depthData) const;

        void getResolution(unsigned int& width, unsigned int& height) const;

    private:
        MaskedOcclusionCulling* mMOC;
        osg::Matrixd mViewProjection;
        float mVPFloat[16] = {};
        bool mFrameActive = false;
        float mTestMarginPx = 0.f;
        osg::Vec3f mFrameEye;
        osg::Matrixd mFrameViewInverse;
        unsigned int mOccluderMask = ~0u;
        bool mClipBelow = false;
        float mClipZ = 0.f;
        // scratch for clipped occluders
        std::vector<osg::Vec3f> mClippedPositions;
        std::vector<unsigned int> mClippedIndices;
        unsigned int mBufWidth = 0;
        unsigned int mBufHeight = 0;

        mutable unsigned int mNumOccluded = 0;
        mutable unsigned int mNumTested = 0;
        unsigned int mNumBuildingOccluders = 0;
        unsigned int mNumBuildingTris = 0;
        unsigned int mNumBuildingVerts = 0;

        static constexpr double sMeshBuildBudget = 0.002; // seconds per frame
        unsigned int mMeshBuildsThisFrame = 0;
        double mMeshBuildSeconds = 0.0;
    };
}

#endif
