#pragma once
#include "DspUtils.h"

namespace naturalair {

// Frame-rate reduction smoother: fast attack, two-stage release, slew-limited.
class EnvelopeSmoother
{
public:
    void prepare(float frameRateHz) { fr = frameRateHz; setTimes(0.5f, 50.f); reset(); }

    // attackMs / releaseMs are fully user controlled. The release has two stages: a fast one
    // (release/4) that drops the peak and a slow one (release) for the natural tail.
    // Note: the gain is updated once per hop (~1.3 ms at 48 kHz), so attack times below that
    // are effectively "instant"; the lookahead already starts the reduction before the onset.
    void setTimes(float attackMs, float releaseMs)
    {
        const float att = std::max(0.00001f, attackMs * 0.001f);
        const float rel = std::max(0.0005f, releaseMs * 0.001f);
        const float frameMs = 1000.f / fr;
        aAtt = 1.f - std::exp(-1.f / (att * fr));
        aRelF = std::exp(-1.f / (0.25f * rel * fr));
        aRelS = std::exp(-1.f / (rel * fr));
        maxAttStep = 60.f * frameMs;     // slew limits are effectively off: the knobs decide
        maxRelStep = 60.f * frameMs;
    }

    void reset() { yF = yS = y = 0.f; }

    // evidence (0..1): how much sibilance is still present around this frame. With no sibilance left
    // the release is shortened (to 15% of the knob) so the vowel that follows is never dulled; while
    // the "s" is still decaying the release is the full value you set.
    float process(float target, float evidence = 1.f)
    {
        const float inv = 1.f / (0.15f + 0.85f * clampf(evidence, 0.f, 1.f));
        const float aF = std::pow(aRelF, inv), aS = std::pow(aRelS, inv);
        auto stage = [&](float& st, float aRel)
        {
            if (target > st) st += (target - st) * aAtt;
            else st = target + (st - target) * aRel;
        };
        stage(yF, aF);
        stage(yS, aS);
        float out = 0.6f * yF + 0.4f * yS;
        if (out > y) out = std::min(out, y + maxAttStep);
        else         out = std::max(out, y - maxRelStep);
        y = out;
        return y;
    }

private:
    float fr = 375.f, aAtt = 1.f, aRelF = 0.9f, aRelS = 0.97f, maxRelStep = 2.5f, maxAttStep = 10.f;
    float yF = 0.f, yS = 0.f, y = 0.f;
};

} // namespace naturalair
