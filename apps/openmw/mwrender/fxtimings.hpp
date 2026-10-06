#ifndef OPENMW_MWRENDER_FXTIMINGS_H
#define OPENMW_MWRENDER_FXTIMINGS_H

#include <string>
#include <vector>

// OpenMGE XE: GPU time of each post-processing shader, measured with GL timestamp queries around each technique in
// PingPongCanvas while the performance log is on, and written out by it as postprocess-profile.txt.
namespace MWRender::FxTimings
{
    void setEnabled(bool enabled);
    bool isEnabled();

    // One technique's GPU time in one frame (draw thread).
    void record(const std::string& technique, double ms);

    struct Entry
    {
        std::string mTechnique;
        double mAverageMs = 0.0; // over the frames it ran in
        double mWorstMs = 0.0;
        unsigned int mFrames = 0;
    };

    // Everything recorded since the last call, slowest first; resets the totals.
    std::vector<Entry> take();
}

#endif
