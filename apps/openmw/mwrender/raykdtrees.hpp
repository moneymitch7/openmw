#ifndef OPENMW_MWRENDER_RAYKDTREES_H
#define OPENMW_MWRENDER_RAYKDTREES_H

#include <memory>
#include <unordered_map>
#include <vector>

#include <osg/observer_ptr>
#include <osg/ref_ptr>

namespace osg
{
    class Drawable;
    class Geometry;
}

namespace SceneUtil
{
    class WorkQueue;
}

namespace MWRender
{
    /// Search trees (osg::KdTree) for the big static meshes that rendering rays reach, so a ray tests the few
    /// triangles near it instead of every triangle of a terrain chunk or a merged chunk of objects. Without one, a
    /// ray costs about 8 ns per triangle of every mesh whose bounds it enters: tens of microseconds for a terrain
    /// chunk, milliseconds for a merged town chunk. With one, about a microsecond.
    ///
    /// Trees are built on demand in the background, the first time a ray reaches a mesh, and attached on the main
    /// thread (the only thread that casts rendering rays); until then the ray tests the mesh the old way. A tree
    /// lives as long as its mesh, and only meshes rays actually reach get one.
    class RayKdTrees
    {
    public:
        RayKdTrees();
        ~RayKdTrees();

        /// A ray reached this drawable: queue a tree for it if it is a big static triangle mesh without one.
        void visit(osg::Drawable& drawable, SceneUtil::WorkQueue* queue);

        /// Attach the trees finished since the last call. Main thread only, before a ray is cast.
        void collect();

    private:
        struct Job;

        std::vector<std::shared_ptr<Job>> mPending;
        // meshes a tree could not be built for, so they are not queued again
        std::unordered_map<const osg::Geometry*, osg::observer_ptr<osg::Geometry>> mRejected;
    };
}

#endif
