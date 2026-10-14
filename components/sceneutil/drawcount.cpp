#include "drawcount.hpp"

#include <atomic>
#include <mutex>

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osgUtil/CullVisitor>

namespace SceneUtil::DrawCount
{
    namespace
    {
        // The node masks of apps/openmw/mwrender/vismask.hpp that tell what a drawable belongs to.
        constexpr unsigned int sMaskEffect = 1u << 1;
        constexpr unsigned int sMaskActor = 1u << 3;
        constexpr unsigned int sMaskPlayer = 1u << 4;
        constexpr unsigned int sMaskSky = 1u << 5;
        constexpr unsigned int sMaskWater = 1u << 6;
        constexpr unsigned int sMaskSimpleWater = 1u << 7;
        constexpr unsigned int sMaskTerrain = 1u << 8;
        constexpr unsigned int sMaskFirstPerson = 1u << 9;
        constexpr unsigned int sMaskObject = 1u << 10;
        constexpr unsigned int sMaskStatic = 1u << 11;
        constexpr unsigned int sMaskWeatherParticles = 1u << 13;
        constexpr unsigned int sMaskParticleSystem = 1u << 16;
        constexpr unsigned int sMaskGroundcover = 1u << 20;

        constexpr unsigned int sSampleEvery = 10;

        using Counts = std::array<unsigned int, sNumCategories>;

        std::atomic<bool> sEnabled{ false };
        std::mutex sLatestMutex;
        Counts sLatest{};
        bool sHaveLatest = false;
        // The frame being added up: every camera of a frame has its own cull visitor (the scene, the interface, ...),
        // each adds what it drew once it is used again.
        unsigned int sSumFrame = 0;
        Counts sSum{};
        bool sHaveSum = false;

        void addCounts(unsigned int frame, const Counts& counts)
        {
            const std::lock_guard lock(sLatestMutex);
            if (sHaveSum && frame == sSumFrame)
            {
                for (std::size_t i = 0; i < sNumCategories; ++i)
                    sSum[i] += counts[i];
                return;
            }
            if (sHaveSum && frame < sSumFrame)
                return; // a visitor not used since an older counted frame
            if (sHaveSum)
            {
                sLatest = sSum;
                sHaveLatest = true;
            }
            sSum = counts;
            sSumFrame = frame;
            sHaveSum = true;
        }

        // What a node itself says the drawables below it are, if anything.
        bool nodeCategory(const osg::Node& node, Category& category)
        {
            const std::string& name = node.getName();
            if (!name.empty())
            {
                if (name == sActiveGridChunkName)
                {
                    category = Category::NearbyMerged;
                    return true;
                }
                if (name == sDistantChunkName)
                {
                    category = Category::DistantObjects;
                    return true;
                }
            }
            const unsigned int mask = node.getNodeMask();
            if (mask == ~0u || mask == 0u)
                return false;
            if (mask & sMaskGroundcover)
                category = Category::Grass;
            else if (mask & (sMaskActor | sMaskPlayer | sMaskFirstPerson))
                category = Category::Actors;
            else if (mask & sMaskTerrain)
                category = Category::Land;
            else if (mask & (sMaskObject | sMaskStatic))
                category = Category::Objects;
            else if (mask
                & (sMaskEffect | sMaskSky | sMaskWater | sMaskSimpleWater | sMaskWeatherParticles
                    | sMaskParticleSystem))
                category = Category::Other;
            else
                return false;
            return true;
        }

        Category classify(const osg::NodePath& path)
        {
            bool found = false;
            Category category = Category::Other;
            for (auto it = path.rbegin(); it != path.rend(); ++it)
            {
                const osg::Node* node = *it;
                if (node == nullptr)
                    continue;
                if (const osg::Camera* camera = node->asCamera())
                {
                    // The innermost camera is the pass the drawable is drawn in.
                    const std::string& name = camera->getName();
                    if (name == "ShadowCamera")
                        return Category::ShadowMaps;
                    if (name == "ReflectionCamera")
                        return Category::Reflection;
                    if (name == "SceneCam")
                        return found ? category : Category::Other;
                    return Category::OtherPasses;
                }
                if (!found)
                    found = nodeCategory(*node, category);
            }
            return found ? category : Category::Other;
        }

        class CountingCullVisitor : public osgUtil::CullVisitor
        {
        public:
            CountingCullVisitor() = default;

            CountingCullVisitor(const CountingCullVisitor& copy)
                : osg::Object(copy)
                , osgUtil::CullVisitor(copy)
            {
            }

            osgUtil::CullVisitor* clone() const override { return new CountingCullVisitor(*this); }

            using osgUtil::CullVisitor::apply;

            void reset() override
            {
                osgUtil::CullVisitor::reset();
                // What this visitor counted in its last use is complete now.
                if (mCounting)
                    addCounts(mCountFrame, mCounts);
                mCounting = false;
            }

            void apply(osg::Drawable& drawable) override
            {
                if (!mCounting)
                {
                    const osg::FrameStamp* frameStamp = getFrameStamp();
                    if (!sEnabled.load(std::memory_order_relaxed) || frameStamp == nullptr
                        || frameStamp->getFrameNumber() % sSampleEvery != 0)
                    {
                        osgUtil::CullVisitor::apply(drawable);
                        return;
                    }
                    mCounting = true;
                    mCountFrame = frameStamp->getFrameNumber();
                    mCounts.fill(0);
                }
                // Every drawable kept for drawing takes a render leaf.
                const unsigned int before = _currentReuseRenderLeafIndex;
                osgUtil::CullVisitor::apply(drawable);
                const unsigned int added = _currentReuseRenderLeafIndex - before;
                if (added != 0)
                    mCounts[static_cast<std::size_t>(classify(getNodePath()))] += added;
            }

        private:
            bool mCounting = false; // counting the frame mCountFrame
            unsigned int mCountFrame = 0;
            Counts mCounts{};
        };
    }

    std::string_view getStatName(Category category)
    {
        switch (category)
        {
            case Category::Land:
                return "Draws Land";
            case Category::DistantObjects:
                return "Draws Distant Objects";
            case Category::NearbyMerged:
                return "Draws Nearby Merged";
            case Category::Objects:
                return "Draws Objects";
            case Category::Actors:
                return "Draws Actors";
            case Category::Grass:
                return "Draws Grass";
            case Category::Other:
                return "Draws Other";
            case Category::ShadowMaps:
                return "Draws Shadow Maps";
            case Category::Reflection:
                return "Draws Reflection";
            case Category::OtherPasses:
                return "Draws Other Passes";
            case Category::Count:
                break;
        }
        return "Draws";
    }

    void installCountingCullVisitor(bool enabled)
    {
        sEnabled = enabled;
        if (enabled)
            osgUtil::CullVisitor::prototype() = new CountingCullVisitor;
    }

    bool getLatest(std::array<unsigned int, sNumCategories>& counts)
    {
        const std::lock_guard lock(sLatestMutex);
        counts = sLatest;
        return sHaveLatest;
    }
}
