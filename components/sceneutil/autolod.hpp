#ifndef OPENMW_COMPONENTS_SCENEUTIL_AUTOLOD_H
#define OPENMW_COMPONENTS_SCENEUTIL_AUTOLOD_H

#include <osg/Node>
#include <osg/ref_ptr>

namespace SceneUtil
{
    struct AutoLodStats
    {
        unsigned int mTrianglesBefore = 0;
        unsigned int mTrianglesAfter = 0;
    };

    /// OpenMGE XE automatic LOD: a copy of @a node whose plain triangle meshes are simplified with meshoptimizer so
    /// that no surface moves by more than @a maxError (in the node's own units). Open borders are kept in place so
    /// modular pieces (walls, cantons) don't open cracks between each other, and texture seams are kept.
    ///
    /// Nodes are copied; meshes left alone (skinned, morphed, particles, small, or barely reducible ones) are shared
    /// with @a node, simplified ones are new geometry with their own compacted arrays, so nothing of @a node changes.
    ///
    /// Nodes not matching @a mask (collision shapes, hidden nodes) are shared without looking inside.
    ///
    /// @return nullptr if nothing could be simplified usefully (use @a node itself then).
    osg::ref_ptr<osg::Node> createSimplifiedCopy(const osg::Node& node, float maxError, unsigned int minTriangles,
        AutoLodStats* stats = nullptr, osg::Node::NodeMask mask = ~0u);

    /// Time spent in createSimplifiedCopy so far, on all threads together, in microseconds.
    unsigned long long getAutoLodMicroseconds();
}

#endif
