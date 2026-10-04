#ifndef OPENMW_COMPONENTS_TERRAIN_TERRAINOCCLUDER_H
#define OPENMW_COMPONENTS_TERRAIN_TERRAINOCCLUDER_H

#include <osg/Vec2f>
#include <osg/Vec2i>
#include <osg/Vec3f>

#include <components/esm/refid.hpp>

#include <map>
#include <utility>
#include <vector>

namespace Terrain
{
    class Storage;

    /// Generates conservative terrain geometry for software occlusion rasterization.
    /// Fetches full-resolution heights and min-pools them to a coarse grid, ensuring
    /// the occluder mesh is always at or below the actual terrain surface.
    ///
    /// Runs on the cull thread, so the work is spread out: each cell's coarse mesh is built
    /// once and cached, and at most a few new cells are built per frame (nearest first).
    /// A cell that is not built yet is simply missing from the occluder for a frame or two,
    /// which only means less culling, never wrongly hidden objects.
    class TerrainOccluder
    {
    public:
        TerrainOccluder(Storage* storage, float cellWorldSize);

        void setWorldspace(ESM::RefId worldspace);
        void setLodLevel(int lod);

        /// Build occluder geometry for terrain cells around the camera.
        /// Generates world-space positions and triangle indices.
        /// @param eyePoint Camera position in world space
        /// @param radius Radius in cells to include
        void build(const osg::Vec3f& eyePoint, int radiusCells, std::vector<osg::Vec3f>& outPositions,
            std::vector<unsigned int>& outIndices);

        bool hasTerrainData() const;

    private:
        struct CellMesh
        {
            std::vector<osg::Vec3f> mPositions; // world space
            std::vector<unsigned int> mIndices; // local to mPositions
        };

        void buildCell(int cellX, int cellY, CellMesh& out);

        Storage* mStorage;
        ESM::RefId mWorldspace;
        float mCellWorldSize;
        int mLodLevel = 3;
        unsigned int mMaxNewCellsPerFrame = 2;

        std::map<std::pair<int, int>, CellMesh> mCells;

        // Scratch buffer for min-height pooling (reused across cells)
        std::vector<float> mQuadMins;
    };
}

#endif
