#include "occlusionculling.hpp"

#include <MaskedOcclusionCulling.h>

#include <algorithm>
#include <cmath>

namespace SceneUtil
{
    OcclusionCuller::OcclusionCuller(unsigned int bufferWidth, unsigned int bufferHeight)
        : mMOC(nullptr)
    {
        mMOC = MaskedOcclusionCulling::Create();
        if (mMOC)
        {
            // Width must be multiple of 8, height multiple of 4
            bufferWidth = (bufferWidth + 7) & ~7u;
            bufferHeight = (bufferHeight + 3) & ~3u;
            mMOC->SetResolution(bufferWidth, bufferHeight);
            mBufWidth = bufferWidth;
            mBufHeight = bufferHeight;
            mMOC->SetNearClipPlane(0.1f);
        }
    }

    OcclusionCuller::~OcclusionCuller()
    {
        if (mMOC)
            MaskedOcclusionCulling::Destroy(mMOC);
    }

    void OcclusionCuller::beginFrame(
        const osg::Matrixd& viewMatrix, const osg::Matrixd& projectionMatrix, bool resetStats)
    {
        mFrameActive = false;
        mClipBelow = false;
        if (!mMOC)
            return;

        mMOC->ClearBuffer();
        mViewProjection = viewMatrix * projectionMatrix;

        const double* vpDouble = mViewProjection.ptr();
        for (int i = 0; i < 16; ++i)
            mVPFloat[i] = static_cast<float>(vpDouble[i]);

        if (resetStats)
        {
            mNumOccluded = 0;
            mNumTested = 0;
            mMeshBuildsThisFrame = 0;
            mMeshBuildSeconds = 0.0;
        }
        // per view: the triangle budget is counted with these
        mNumBuildingOccluders = 0;
        mNumBuildingTris = 0;
        mNumBuildingVerts = 0;
        mFrameActive = true;
    }

    namespace
    {
        /// Appends the part of triangle (a, b, c) at or above height z, as 0, 1 or 2 triangles.
        void clipTriangleBelow(const osg::Vec3f& a, const osg::Vec3f& b, const osg::Vec3f& c, float z,
            std::vector<osg::Vec3f>& positions, std::vector<unsigned int>& indices)
        {
            const osg::Vec3f in[3] = { a, b, c };
            osg::Vec3f out[4];
            int numOut = 0;
            for (int i = 0; i < 3; ++i)
            {
                const osg::Vec3f& p = in[i];
                const osg::Vec3f& q = in[(i + 1) % 3];
                const bool pAbove = p.z() >= z;
                const bool qAbove = q.z() >= z;
                if (pAbove)
                    out[numOut++] = p;
                if (pAbove != qAbove)
                {
                    const float t = (z - p.z()) / (q.z() - p.z());
                    out[numOut++] = p + (q - p) * t;
                }
            }
            if (numOut < 3)
                return;
            const unsigned int base = static_cast<unsigned int>(positions.size());
            for (int i = 0; i < numOut; ++i)
                positions.push_back(out[i]);
            for (int i = 1; i + 1 < numOut; ++i)
            {
                indices.push_back(base);
                indices.push_back(base + i);
                indices.push_back(base + i + 1);
            }
        }
    }

    void OcclusionCuller::rasterizeOccluder(
        const std::vector<osg::Vec3f>& worldPositionsIn, const std::vector<unsigned int>& indicesIn)
    {
        if (!mFrameActive || worldPositionsIn.empty() || indicesIn.empty())
            return;

        const std::vector<osg::Vec3f>* positionsPtr = &worldPositionsIn;
        const std::vector<unsigned int>* indicesPtr = &indicesIn;
        if (mClipBelow)
        {
            // only what is above the clip height occludes (see setOccluderClipBelow)
            bool anyBelow = false;
            bool anyAbove = false;
            for (const auto& v : worldPositionsIn)
            {
                if (v.z() < mClipZ)
                    anyBelow = true;
                else
                    anyAbove = true;
            }
            if (!anyAbove)
                return;
            if (anyBelow)
            {
                const unsigned int vertexCount = static_cast<unsigned int>(worldPositionsIn.size());
                mClippedPositions.clear();
                mClippedIndices.clear();
                for (size_t i = 0; i + 2 < indicesIn.size(); i += 3)
                {
                    const unsigned int ia = indicesIn[i], ib = indicesIn[i + 1], ic = indicesIn[i + 2];
                    if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount)
                        return;
                    clipTriangleBelow(worldPositionsIn[ia], worldPositionsIn[ib], worldPositionsIn[ic], mClipZ,
                        mClippedPositions, mClippedIndices);
                }
                if (mClippedIndices.empty())
                    return;
                positionsPtr = &mClippedPositions;
                indicesPtr = &mClippedIndices;
            }
        }
        const std::vector<osg::Vec3f>& worldPositions = *positionsPtr;
        const std::vector<unsigned int>& indices = *indicesPtr;

        const int numTris = static_cast<int>(indices.size()) / 3;
        if (numTris <= 0)
            return;

        // Validate all vertex indices are in range to prevent out-of-bounds reads.
        const unsigned int vertexCount = static_cast<unsigned int>(worldPositions.size());
        for (const auto idx : indices)
        {
            if (idx >= vertexCount)
                return;
        }

        // Pre-transform check: skip triangles with vertices that would produce
        // extreme clip-space coordinates (w near zero after VP transform).
        for (const auto& v : worldPositions)
        {
            float w = mVPFloat[3] * v.x() + mVPFloat[7] * v.y() + mVPFloat[11] * v.z() + mVPFloat[15];
            if (!std::isfinite(w) || std::abs(w) < 1e-6f)
                return;
        }

        // Vec3f layout: stride=12 bytes, yOffset=4, zOffset=8
        // MOC treats (x,y,z) as (x,y,w_component) and transforms via the VP matrix
        MaskedOcclusionCulling::VertexLayout vtxLayout(12, 4, 8);

        mMOC->RenderTriangles(reinterpret_cast<const float*>(worldPositions.data()), indices.data(), numTris, mVPFloat,
            MaskedOcclusionCulling::BACKFACE_NONE, // terrain can be seen from below at edges
            MaskedOcclusionCulling::CLIP_PLANE_ALL, vtxLayout);
    }

    void OcclusionCuller::rasterizeAABBOccluder(const osg::BoundingBox& worldBBIn)
    {
        if (!mFrameActive)
            return;
        osg::BoundingBox worldBB = worldBBIn;
        if (mClipBelow)
        {
            if (worldBB.zMax() <= mClipZ)
                return;
            worldBB.zMin() = std::max(worldBB.zMin(), mClipZ);
        }

        // 8 corners of the AABB
        const osg::Vec3f verts[8] = {
            osg::Vec3f(worldBB.xMin(), worldBB.yMin(), worldBB.zMin()), // 0
            osg::Vec3f(worldBB.xMax(), worldBB.yMin(), worldBB.zMin()), // 1
            osg::Vec3f(worldBB.xMin(), worldBB.yMax(), worldBB.zMin()), // 2
            osg::Vec3f(worldBB.xMax(), worldBB.yMax(), worldBB.zMin()), // 3
            osg::Vec3f(worldBB.xMin(), worldBB.yMin(), worldBB.zMax()), // 4
            osg::Vec3f(worldBB.xMax(), worldBB.yMin(), worldBB.zMax()), // 5
            osg::Vec3f(worldBB.xMin(), worldBB.yMax(), worldBB.zMax()), // 6
            osg::Vec3f(worldBB.xMax(), worldBB.yMax(), worldBB.zMax()), // 7
        };

        // 12 triangles (6 faces x 2 tris)
        static const unsigned int indices[36] = {
            // -Z face
            0,
            1,
            3,
            0,
            3,
            2,
            // +Z face
            4,
            6,
            7,
            4,
            7,
            5,
            // -Y face
            0,
            4,
            5,
            0,
            5,
            1,
            // +Y face
            2,
            3,
            7,
            2,
            7,
            6,
            // -X face
            0,
            2,
            6,
            0,
            6,
            4,
            // +X face
            1,
            5,
            7,
            1,
            7,
            3,
        };

        MaskedOcclusionCulling::VertexLayout vtxLayout(12, 4, 8);

        mMOC->RenderTriangles(reinterpret_cast<const float*>(verts), indices, 12, mVPFloat,
            MaskedOcclusionCulling::BACKFACE_NONE, // both sides, nearest depth wins
            MaskedOcclusionCulling::CLIP_PLANE_ALL, vtxLayout);

        ++mNumBuildingOccluders;
    }

    bool OcclusionCuller::testVisibleAABB(const osg::BoundingBox& worldBB) const
    {
        if (!mFrameActive)
            return true;

        ++mNumTested;

        // Project all 8 AABB corners to clip space, find NDC screen rect + minimum w
        const osg::Vec3f corners[8] = {
            osg::Vec3f(worldBB.xMin(), worldBB.yMin(), worldBB.zMin()),
            osg::Vec3f(worldBB.xMax(), worldBB.yMin(), worldBB.zMin()),
            osg::Vec3f(worldBB.xMin(), worldBB.yMax(), worldBB.zMin()),
            osg::Vec3f(worldBB.xMax(), worldBB.yMax(), worldBB.zMin()),
            osg::Vec3f(worldBB.xMin(), worldBB.yMin(), worldBB.zMax()),
            osg::Vec3f(worldBB.xMax(), worldBB.yMin(), worldBB.zMax()),
            osg::Vec3f(worldBB.xMin(), worldBB.yMax(), worldBB.zMax()),
            osg::Vec3f(worldBB.xMax(), worldBB.yMax(), worldBB.zMax()),
        };

        float ndcMinX = 1.0f, ndcMinY = 1.0f;
        float ndcMaxX = -1.0f, ndcMaxY = -1.0f;
        float wMin = std::numeric_limits<float>::max();
        bool anyInFront = false;

        const double* m = mViewProjection.ptr();
        for (int i = 0; i < 8; ++i)
        {
            // Transform to clip space: pos * ViewProjection
            double x = corners[i].x(), y = corners[i].y(), z = corners[i].z();
            float cx = static_cast<float>(x * m[0] + y * m[4] + z * m[8] + m[12]);
            float cy = static_cast<float>(x * m[1] + y * m[5] + z * m[9] + m[13]);
            float cw = static_cast<float>(x * m[3] + y * m[7] + z * m[11] + m[15]);

            if (cw > 0.0f)
            {
                anyInFront = true;
                float invW = 1.0f / cw;
                float ndcX = cx * invW;
                float ndcY = cy * invW;
                ndcMinX = std::min(ndcMinX, ndcX);
                ndcMinY = std::min(ndcMinY, ndcY);
                ndcMaxX = std::max(ndcMaxX, ndcX);
                ndcMaxY = std::max(ndcMaxY, ndcY);
                wMin = std::min(wMin, cw);
            }
            else
            {
                // Corner is behind camera, conservatively expand to full screen
                ndcMinX = -1.0f;
                ndcMinY = -1.0f;
                ndcMaxX = 1.0f;
                ndcMaxY = 1.0f;
                anyInFront = true; // still test it
                wMin = std::min(wMin, 0.0001f);
            }
        }

        if (!anyInFront)
            return true; // entirely behind camera, let frustum culling handle it

        // Safety margin:
        // inflate the tested rect by N buffer pixels per side. Borderline
        // objects at occluder silhouettes then stay visible - the coarse
        // buffer (512x256 default) plus vertex-clustered occluder meshes
        // can otherwise claim occlusion for objects that peek out by a
        // sliver, which pops/flickers with sub-pixel camera motion.
        if (mTestMarginPx > 0.f && mBufWidth > 0 && mBufHeight > 0)
        {
            const float mx = mTestMarginPx * 2.0f / static_cast<float>(mBufWidth);
            const float my = mTestMarginPx * 2.0f / static_cast<float>(mBufHeight);
            ndcMinX -= mx;
            ndcMaxX += mx;
            ndcMinY -= my;
            ndcMaxY += my;
        }

        // Clamp to NDC range
        ndcMinX = std::max(ndcMinX, -1.0f);
        ndcMinY = std::max(ndcMinY, -1.0f);
        ndcMaxX = std::min(ndcMaxX, 1.0f);
        ndcMaxY = std::min(ndcMaxY, 1.0f);

        if (ndcMinX >= ndcMaxX || ndcMinY >= ndcMaxY)
            return true; // degenerate rect, assume visible

        auto result = mMOC->TestRect(ndcMinX, ndcMinY, ndcMaxX, ndcMaxY, wMin);

        if (result == MaskedOcclusionCulling::OCCLUDED)
        {
            ++mNumOccluded;
            return false;
        }
        return true;
    }

    void OcclusionCuller::computePixelDepthBuffer(float* depthData) const
    {
        if (mMOC)
            mMOC->ComputePixelDepthBuffer(depthData, true); // flipY for OpenGL bottom-to-top
    }

    void OcclusionCuller::getResolution(unsigned int& width, unsigned int& height) const
    {
        if (mMOC)
            mMOC->GetResolution(width, height);
        else
        {
            width = 0;
            height = 0;
        }
    }
}
