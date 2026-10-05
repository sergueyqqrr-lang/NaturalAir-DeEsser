#pragma once
#include "DspUtils.h"

namespace naturalair {

// Reduction law. Excess over the threshold (soft knee) is mapped onto 0..Range with an ease-out
// curve, so:
//   - Threshold decides WHERE reduction starts (and how fast it ramps up),
//   - Range decides how much reduction a clearly excessive "s" gets (it scales the whole curve,
//     it is not just a cap that is never reached),
//   - an "s" is never pushed below a natural floor vs. the vowel (prevents the "lispy" sound).
struct GainComputer
{
    static constexpr float kKneeDb = 6.f;
    static constexpr float kFullExcessDb = 12.f;     // excess that earns the full Range
    static constexpr float kFloorRatioDb = -18.f;

    static float softKnee(float x)
    {
        const float W = kKneeDb;
        if (2.f * x < -W) return 0.f;
        if (2.f * std::fabs(x) <= W) { const float t = x + W * 0.5f; return t * t / (2.f * W); }
        return x;
    }

    static float reduction(float ratioDb, float thresholdDb, float rangeDb)
    {
        const float u = clampf(softKnee(ratioDb - thresholdDb) / kFullExcessDb, 0.f, 1.f);
        float R = rangeDb * u * (2.f - u);
        R = std::min(R, std::max(0.f, ratioDb - kFloorRatioDb));
        return std::max(0.f, R);
    }
};

} // namespace naturalair
