#include "skeleton.hpp"

#include <osg/MatrixTransform>

#include <components/debug/debuglog.hpp>
#include <components/misc/strings/lower.hpp>

#include "lightmanager.hpp"

#include <algorithm>

namespace SceneUtil
{

    class InitBoneCacheVisitor : public osg::NodeVisitor
    {
    public:
        typedef std::vector<osg::MatrixTransform*> TransformPath;
        InitBoneCacheVisitor(std::unordered_map<std::string, TransformPath>& cache)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mCache(cache)
        {
        }

        void apply(osg::MatrixTransform& node) override
        {
            mPath.push_back(&node);
            mCache.emplace(Misc::StringUtils::lowerCase(node.getName()), mPath);
            traverse(node);
            mPath.pop_back();
        }

    private:
        TransformPath mPath;
        std::unordered_map<std::string, TransformPath>& mCache;
    };

    Skeleton::Skeleton()
        : mBoneCacheInit(false)
        , mNeedToUpdateBoneMatrices(true)
        , mActive(Active)
        , mLastFrameNumber(0)
        , mLastCullFrameNumber(0)
    {
    }

    Skeleton::Skeleton(const Skeleton& copy, const osg::CopyOp& copyop)
        : osg::Group(copy, copyop)
        , mBoneCacheInit(false)
        , mNeedToUpdateBoneMatrices(true)
        , mActive(copy.mActive)
        , mLastFrameNumber(0)
        , mLastCullFrameNumber(0)
    {
    }

    Bone* Skeleton::getBone(const std::string& name)
    {
        if (!mBoneCacheInit)
        {
            InitBoneCacheVisitor visitor(mBoneCache);
            accept(visitor);
            mBoneCacheInit = true;
        }

        BoneCache::iterator found = mBoneCache.find(Misc::StringUtils::lowerCase(name));
        if (found == mBoneCache.end())
            return nullptr;

        // find or insert in the bone hierarchy

        if (!mRootBone.get())
        {
            mRootBone = std::make_unique<Bone>();
        }

        Bone* bone = mRootBone.get();
        for (osg::MatrixTransform* matrixTransform : found->second)
        {
            const auto it = std::find_if(bone->mChildren.begin(), bone->mChildren.end(),
                [&](const auto& v) { return v->mNode == matrixTransform; });

            if (it == bone->mChildren.end())
            {
                bone = bone->mChildren.emplace_back(std::make_unique<Bone>()).get();
                mNeedToUpdateBoneMatrices = true;
            }
            else
                bone = it->get();

            bone->mNode = matrixTransform;
        }

        return bone;
    }

    void Skeleton::updateBoneMatrices(unsigned int traversalNumber)
    {
        if (traversalNumber != mLastFrameNumber)
            mNeedToUpdateBoneMatrices = true;

        mLastFrameNumber = traversalNumber;

        if (mNeedToUpdateBoneMatrices)
        {
            if (mRootBone.get())
            {
                for (const auto& child : mRootBone->mChildren)
                    child->update(nullptr);
            }

            mNeedToUpdateBoneMatrices = false;
        }
    }

    void Skeleton::setActive(ActiveType active)
    {
        mActive = active;
    }

    bool Skeleton::getActive() const
    {
        return mActive != Inactive;
    }

    void Skeleton::markDirty()
    {
        mLastFrameNumber = 0;
        mBoneCache.clear();
        mBoneCacheInit = false;
    }

    namespace
    {
        class FindLightSourcesVisitor : public osg::NodeVisitor
        {
        public:
            explicit FindLightSourcesVisitor(std::vector<std::vector<osg::observer_ptr<osg::Node>>>& paths)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mPaths(paths)
            {
            }

            void apply(osg::Node& node) override
            {
                if (dynamic_cast<LightSource*>(&node) != nullptr)
                {
                    // the path starts at the skeleton, which is left out
                    const osg::NodePath& path = getNodePath();
                    std::vector<osg::observer_ptr<osg::Node>>& lightPath = mPaths.emplace_back();
                    for (std::size_t i = 1; i < path.size(); ++i)
                        lightPath.emplace_back(path[i]);
                }
                traverse(node);
            }

        private:
            std::vector<std::vector<osg::observer_ptr<osg::Node>>>& mPaths;
        };
    }

    void Skeleton::findLights(unsigned int traversalNumber)
    {
        mLightPaths.clear();
        FindLightSourcesVisitor visitor(mLightPaths);
        accept(visitor);
        mLightPathsFrame = traversalNumber;
        mLightPathsValid = true;
    }

    void Skeleton::updateLightsOnly(osg::NodeVisitor& nv)
    {
        // A torch equipped while off-screen is found within half a second.
        constexpr unsigned int refreshInterval = 30;
        const unsigned int frame = nv.getTraversalNumber();
        if (!mLightPathsValid || frame - mLightPathsFrame >= refreshInterval)
            findLights(frame);

        std::vector<osg::ref_ptr<osg::Node>> nodes;
        for (const auto& lightPath : mLightPaths)
        {
            // still attached the way it was found, and not hidden
            nodes.clear();
            const osg::Node* parent = this;
            bool valid = !lightPath.empty();
            for (const auto& observer : lightPath)
            {
                osg::ref_ptr<osg::Node> node;
                if (!observer.lock(node))
                {
                    valid = false;
                    break;
                }
                const osg::Node::ParentList& parents = node->getParents();
                if (std::find(parents.begin(), parents.end(), parent) == parents.end())
                {
                    valid = false;
                    break;
                }
                parent = node.get();
                nodes.push_back(std::move(node));
            }
            if (!valid)
            {
                mLightPathsValid = false;
                continue;
            }
            bool visible = true;
            for (const auto& node : nodes)
                visible = visible && nv.validNodeMask(*node);
            if (!visible)
                continue;

            auto* light = static_cast<LightSource*>(nodes.back().get());
            osg::Callback* callback = light->getUpdateCallback();
            if (callback == nullptr)
                continue;
            // the update callbacks of the light (collecting it, its flicker) run as in a full update traversal
            for (const auto& node : nodes)
                nv.pushOntoNodePath(node.get());
            callback->run(light, &nv);
            for (std::size_t i = 0; i < nodes.size(); ++i)
                nv.popFromNodePath();
        }
    }

    void Skeleton::traverse(osg::NodeVisitor& nv)
    {
        if (nv.getVisitorType() == osg::NodeVisitor::UPDATE_VISITOR)
        {
            if (mActive == Inactive && mLastFrameNumber != 0)
                return;
            if (mActive == SemiActive && mLastFrameNumber != 0 && mLastCullFrameNumber + 3 <= nv.getTraversalNumber())
            {
                updateLightsOnly(nv);
                return;
            }
        }
        else if (nv.getVisitorType() == osg::NodeVisitor::CULL_VISITOR)
            mLastCullFrameNumber = nv.getTraversalNumber();

        osg::Group::traverse(nv);
    }

    void Skeleton::childInserted(unsigned int)
    {
        markDirty();
    }

    void Skeleton::childRemoved(unsigned int, unsigned int)
    {
        markDirty();
    }

    Bone::Bone()
        : mNode(nullptr)
    {
    }

    void Bone::update(const osg::Matrixf* parentMatrixInSkeletonSpace)
    {
        if (!mNode)
        {
            Log(Debug::Error) << "Error: Bone without node";
            return;
        }
        if (parentMatrixInSkeletonSpace)
            mMatrixInSkeletonSpace = mNode->getMatrix() * (*parentMatrixInSkeletonSpace);
        else
            mMatrixInSkeletonSpace = mNode->getMatrix();

        for (const auto& child : mChildren)
            child->update(&mMatrixInSkeletonSpace);
    }

}
