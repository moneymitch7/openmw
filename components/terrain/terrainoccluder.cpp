#include "terrainoccluder.hpp"
#include "storage.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include <osg/Array>

namespace Terrain
{
    TerrainOccluder::TerrainOccluder(Storage* storage, float cellWorldSize)
        : mStorage(storage)
        , mCellWorldSize(cellWorldSize)
    {
    }

    void TerrainOccluder::setWorldspace(ESM::RefId worldspace)
    {
        if (worldspace != mWorldspace)
            mCells.clear();
        mWorldspace = worldspace;
    }

    void TerrainOccluder::setLodLevel(int lod)
    {
        if (lod != mLodLevel)
            mCells.clear();
        mLodLevel = lod;
    }

    bool TerrainOccluder::hasTerrainData() const
    {
        // Only exterior worldspaces have terrain
        return !mWorldspace.empty();
    }

    void TerrainOccluder::build(const osg::Vec3f& eyePoint, int radiusCells, std::vector<osg::Vec3f>& outPositions,
        std::vector<unsigned int>& outIndices)
    {
        outPositions.clear();
        outIndices.clear();

        if (!hasTerrainData())
            return;

        const int cellX = static_cast<int>(std::floor(eyePoint.x() / mCellWorldSize));
        const int cellY = static_cast<int>(std::floor(eyePoint.y() / mCellWorldSize));

        unsigned int budget = mMaxNewCellsPerFrame;
        const auto addCell = [&](int cx, int cy) {
            auto it = mCells.find({ cx, cy });
            if (it == mCells.end())
            {
                if (budget == 0)
                    return; // built on a later frame
                --budget;
                it = mCells.emplace(std::make_pair(cx, cy), CellMesh{}).first;
                buildCell(cx, cy, it->second);
            }
            const CellMesh& mesh = it->second;
            const unsigned int base = static_cast<unsigned int>(outPositions.size());
            outPositions.insert(outPositions.end(), mesh.mPositions.begin(), mesh.mPositions.end());
            for (unsigned int index : mesh.mIndices)
                outIndices.push_back(base + index);
        };

        // Nearest cells first, ring by ring, so the cells that matter most are built first.
        for (int r = 0; r <= radiusCells; ++r)
        {
            if (r == 0)
            {
                addCell(cellX, cellY);
                continue;
            }
            for (int d = -r; d <= r; ++d)
            {
                addCell(cellX + d, cellY - r);
                addCell(cellX + d, cellY + r);
            }
            for (int d = -r + 1; d <= r - 1; ++d)
            {
                addCell(cellX - r, cellY + d);
                addCell(cellX + r, cellY + d);
            }
        }

        // Forget cells well outside the radius
        const int keep = radiusCells + 2;
        for (auto it = mCells.begin(); it != mCells.end();)
        {
            if (std::abs(it->first.first - cellX) > keep || std::abs(it->first.second - cellY) > keep)
                it = mCells.erase(it);
            else
                ++it;
        }
    }

    void TerrainOccluder::buildCell(int cx, int cy, CellMesh& out)
    {
        // Fetch full-resolution heights (LOD 0) and min-pool them to the coarse grid, so the
        // occluder is always at or below the actual terrain surface. This prevents false
        // occlusion in valleys and rapid elevation changes.
        const int step = 1 << mLodLevel;
        const osg::Vec2f center(cx + 0.5f, cy + 0.5f);

        osg::ref_ptr<osg::Vec3Array> fullRes(new osg::Vec3Array);
        osg::ref_ptr<osg::Vec3Array> normals(new osg::Vec3Array);
        osg::ref_ptr<osg::Vec4ubArray> colors(new osg::Vec4ubArray);
        colors->setNormalize(true);

        mStorage->fillVertexBuffers(0, 1.0f, center, mWorldspace, *fullRes, *normals, *colors);

        if (fullRes->empty())
            return;

        const int fullPerSide = static_cast<int>(std::sqrt(static_cast<float>(fullRes->size())));
        if (fullPerSide < 2)
            return;

        const int coarsePerSide = (fullPerSide - 1) / step + 1;
        if (coarsePerSide < 2)
            return;

        // Pass 1: compute min height for each coarse quad
        const int numQuads = (coarsePerSide - 1) * (coarsePerSide - 1);
        mQuadMins.resize(numQuads);
        for (int qj = 0; qj < coarsePerSide - 1; ++qj)
        {
            for (int qi = 0; qi < coarsePerSide - 1; ++qi)
            {
                const int startI = qi * step;
                const int startJ = qj * step;
                const int endI = std::min((qi + 1) * step, fullPerSide - 1);
                const int endJ = std::min((qj + 1) * step, fullPerSide - 1);

                float minH = std::numeric_limits<float>::max();
                for (int fj = startJ; fj <= endJ; ++fj)
                    for (int fi = startI; fi <= endI; ++fi)
                        minH = std::min(minH, (*fullRes)[fj * fullPerSide + fi].z());

                mQuadMins[qj * (coarsePerSide - 1) + qi] = minH;
            }
        }

        // Pass 2: each coarse vertex gets the min height of its surrounding quads
        const osg::Vec3f worldOffset(center.x() * mCellWorldSize, center.y() * mCellWorldSize, 0.0f);
        out.mPositions.reserve(coarsePerSide * coarsePerSide);
        for (int cj = 0; cj < coarsePerSide; ++cj)
        {
            for (int ci = 0; ci < coarsePerSide; ++ci)
            {
                float minH = std::numeric_limits<float>::max();
                // A vertex touches up to 4 quads
                for (int dj = -1; dj <= 0; ++dj)
                {
                    for (int di = -1; di <= 0; ++di)
                    {
                        const int qi = ci + di;
                        const int qj = cj + dj;
                        if (qi >= 0 && qi < coarsePerSide - 1 && qj >= 0 && qj < coarsePerSide - 1)
                            minH = std::min(minH, mQuadMins[qj * (coarsePerSide - 1) + qi]);
                    }
                }

                osg::Vec3f pos = (*fullRes)[(cj * step) * fullPerSide + (ci * step)];
                pos.z() = minH;
                out.mPositions.push_back(pos + worldOffset);
            }
        }

        // Triangle indices for the coarse grid
        out.mIndices.reserve((coarsePerSide - 1) * (coarsePerSide - 1) * 6);
        for (int row = 0; row < coarsePerSide - 1; ++row)
        {
            for (int col = 0; col < coarsePerSide - 1; ++col)
            {
                const unsigned int tl = row * coarsePerSide + col;
                const unsigned int tr = tl + 1;
                const unsigned int bl = tl + coarsePerSide;
                const unsigned int br = bl + 1;

                out.mIndices.push_back(tl);
                out.mIndices.push_back(bl);
                out.mIndices.push_back(tr);

                out.mIndices.push_back(tr);
                out.mIndices.push_back(bl);
                out.mIndices.push_back(br);
            }
        }
    }
}
