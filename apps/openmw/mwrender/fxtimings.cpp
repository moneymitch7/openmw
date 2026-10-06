#include "fxtimings.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

namespace MWRender::FxTimings
{
    namespace
    {
        struct Totals
        {
            double mSum = 0.0;
            double mWorst = 0.0;
            unsigned int mFrames = 0;
        };

        std::atomic<bool> sEnabled{ false };
        std::mutex sMutex;
        std::map<std::string, Totals> sTotals;
    }

    void setEnabled(bool enabled)
    {
        sEnabled = enabled;
    }

    bool isEnabled()
    {
        return sEnabled;
    }

    void record(const std::string& technique, double ms)
    {
        const std::lock_guard lock(sMutex);
        Totals& totals = sTotals[technique];
        totals.mSum += ms;
        totals.mWorst = std::max(totals.mWorst, ms);
        ++totals.mFrames;
    }

    std::vector<Entry> take()
    {
        std::map<std::string, Totals> totals;
        {
            const std::lock_guard lock(sMutex);
            totals.swap(sTotals);
        }
        std::vector<Entry> result;
        result.reserve(totals.size());
        for (const auto& [name, value] : totals)
            if (value.mFrames > 0)
                result.push_back({ name, value.mSum / value.mFrames, value.mWorst, value.mFrames });
        std::sort(result.begin(), result.end(),
            [](const Entry& a, const Entry& b) { return a.mAverageMs > b.mAverageMs; });
        return result;
    }
}
