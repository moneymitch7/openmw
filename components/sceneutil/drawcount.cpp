#include "drawcount.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <components/debug/debuglog.hpp>

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
        using Labels = std::unordered_map<std::string, unsigned int>; // object label -> draws in the main view

        std::atomic<bool> sEnabled{ false };
        std::mutex sLatestMutex;
        Counts sLatest{};
        bool sHaveLatest = false;
        // The frame being added up: every camera of a frame has its own cull visitor (the scene, the interface, ...),
        // each adds what it drew once it is used again.
        unsigned int sSumFrame = 0;
        Counts sSum{};
        Labels sSumLabels;
        bool sHaveSum = false;
        Labels sLatestLabels;

        void addCounts(unsigned int frame, const Counts& counts, Labels& labels)
        {
            const std::lock_guard lock(sLatestMutex);
            if (sHaveSum && frame == sSumFrame)
            {
                for (std::size_t i = 0; i < sNumCategories; ++i)
                    sSum[i] += counts[i];
                for (const auto& [label, draws] : labels)
                    sSumLabels[label] += draws;
                return;
            }
            if (sHaveSum && frame < sSumFrame)
                return; // a visitor not used since an older counted frame
            if (sHaveSum)
            {
                sLatest = sSum;
                sLatestLabels.swap(sSumLabels);
                sHaveLatest = true;
            }
            sSum = counts;
            sSumLabels.swap(labels);
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

        /// @param label set to the object label of the drawable's object, if any, when it is drawn in the main view
        Category classify(const osg::NodePath& path, const std::string*& label)
        {
            bool found = false;
            Category category = Category::Other;
            const std::string* nearestLabel = nullptr;
            label = nullptr;
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
                    {
                        label = nearestLabel;
                        return found ? category : Category::Other;
                    }
                    return Category::OtherPasses;
                }
                if (nearestLabel == nullptr && node->getName().starts_with(sObjectLabelPrefix))
                    nearestLabel = &node->getName();
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
                    addCounts(mCountFrame, mCounts, mLabels);
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
                    mLabels.clear();
                }
                // Every drawable kept for drawing takes a render leaf.
                const unsigned int before = _currentReuseRenderLeafIndex;
                osgUtil::CullVisitor::apply(drawable);
                const unsigned int added = _currentReuseRenderLeafIndex - before;
                if (added != 0)
                {
                    const std::string* label = nullptr;
                    const Category category = classify(getNodePath(), label);
                    mCounts[static_cast<std::size_t>(category)] += added;
                    if (category == Category::Objects && label != nullptr)
                        mLabels[*label] += added;
                }
            }

        private:
            bool mCounting = false; // counting the frame mCountFrame
            unsigned int mCountFrame = 0;
            Counts mCounts{};
            Labels mLabels;
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

    bool isEnabled()
    {
        return sEnabled.load(std::memory_order_relaxed);
    }

    std::string makeObjectLabel(
        std::string_view reason, std::string_view kind, std::string_view id, std::string_view instance)
    {
        std::string label(sObjectLabelPrefix);
        label.append(reason).append("|").append(kind).append("|").append(id).append("|").append(instance);
        return label;
    }

    void logObjects()
    {
        if (!isEnabled())
            return;
        static std::chrono::steady_clock::time_point lastLog;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastLog < std::chrono::seconds(15))
            return;

        Labels labels;
        unsigned int objects = 0;
        {
            const std::lock_guard lock(sLatestMutex);
            if (!sHaveLatest)
                return;
            labels = sLatestLabels;
            objects = sLatest[static_cast<std::size_t>(Category::Objects)];
        }
        lastLog = now;
        if (objects == 0)
            return;

        struct Entry
        {
            std::string_view mReason, mKind, mId;
            unsigned int mDraws = 0;
            unsigned int mCopies = 0;
        };
        std::map<std::string_view, unsigned int> byReason, byKind;
        std::map<std::pair<std::string_view, std::string_view>, Entry> byObject; // kind, id
        unsigned int labelled = 0;
        for (const auto& [label, draws] : labels)
        {
            std::string_view rest = std::string_view(label).substr(sObjectLabelPrefix.size());
            const std::size_t first = rest.find('|');
            const std::size_t second = first == std::string_view::npos ? first : rest.find('|', first + 1);
            const std::size_t third = second == std::string_view::npos ? second : rest.rfind('|');
            if (third == std::string_view::npos || third <= second)
                continue;
            Entry entry{ rest.substr(0, first), rest.substr(first + 1, second - first - 1),
                rest.substr(second + 1, third - second - 1) };
            byReason[entry.mReason] += draws;
            byKind[entry.mKind] += draws;
            Entry& object = byObject.try_emplace({ entry.mKind, entry.mId }, entry).first->second;
            object.mDraws += draws;
            ++object.mCopies; // a label per placed object
            labelled += draws;
        }

        const auto sorted = [](const auto& map) {
            std::vector<std::pair<typename std::decay_t<decltype(map)>::key_type, unsigned int>> result;
            for (const auto& [key, value] : map)
                result.emplace_back(key, value);
            std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            return result;
        };

        std::ostringstream stream;
        stream << "Drawn on their own in the main view (" << objects << " draws";
        if (labelled != objects)
            stream << ", " << objects - labelled << " of them not told apart";
        stream << "). By reason:";
        for (const auto& [reason, draws] : sorted(byReason))
            stream << " " << reason << " " << draws << ",";
        stream << " by kind:";
        for (const auto& [kind, draws] : sorted(byKind))
            stream << " " << kind << " " << draws << ",";
        std::vector<Entry> objectsSorted;
        for (const auto& [key, entry] : byObject)
            objectsSorted.push_back(entry);
        std::sort(objectsSorted.begin(), objectsSorted.end(),
            [](const Entry& a, const Entry& b) { return a.mDraws > b.mDraws; });
        stream << " most drawn:";
        const std::size_t shown = std::min<std::size_t>(objectsSorted.size(), 25);
        for (std::size_t i = 0; i < shown; ++i)
        {
            const Entry& entry = objectsSorted[i];
            stream << " " << entry.mId << " (" << entry.mKind << ", " << entry.mReason << ") " << entry.mDraws
                   << " draws from " << entry.mCopies << (entry.mCopies == 1 ? " copy" : " copies");
            stream << (i + 1 < shown ? "," : "");
        }
        Log(Debug::Info) << stream.str();
    }
}
