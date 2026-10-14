#include "drawcount.hpp"

#include <atomic>
#include <mutex>

#include <osg/Camera>
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

        std::atomic<bool> sEnabled{ false };
        std::atomic<unsigned int> sCulls{ 0 };
        std::mutex sLatestMutex;
        std::array<unsigned int, sNumCategories> sLatest{};
        bool sHaveLatest = false;

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
                // The frame this visitor counted last is complete now: it is the latest one.
                if (mSampling)
                {
                    const std::lock_guard lock(sLatestMutex);
                    sLatest = mCounts;
                    sHaveLatest = true;
                }
                mSampling = sEnabled.load(std::memory_order_relaxed)
                    && sCulls.fetch_add(1, std::memory_order_relaxed) % sSampleEvery == 0;
                if (mSampling)
                    mCounts.fill(0);
            }

            void apply(osg::Drawable& drawable) override
            {
                if (!mSampling)
                {
                    osgUtil::CullVisitor::apply(drawable);
                    return;
                }
                // Every drawable kept for drawing takes a render leaf.
                const unsigned int before = _currentReuseRenderLeafIndex;
                osgUtil::CullVisitor::apply(drawable);
                const unsigned int added = _currentReuseRenderLeafIndex - before;
                if (added != 0)
                    mCounts[static_cast<std::size_t>(classify(getNodePath()))] += added;
            }

        private:
            bool mSampling = false;
            std::array<unsigned int, sNumCategories> mCounts{};
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
