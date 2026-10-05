#pragma once
#include "DspUtils.h"

namespace naturalair {

// Tracks the singer's own reference level and the distribution of their
// sibilance-to-body ratio, so the threshold is relative to the performance.
class AdaptiveThreshold
{
public:
    static constexpr int kBins = 64;
    static constexpr float kLo = -24.f, kHi = 12.f;

    void prepare(float frameRateHz)
    {
        aUp = 1.f - std::exp(-1.f / (0.4f * frameRateHz));
        aDown = 1.f - std::exp(-1.f / (2.5f * frameRateHz));
        decay = std::exp(-1.f / (30.f * frameRateHz));
        reset();
    }

    void reset()
    {
        voice = -200.f;
        init = false;
        total = 0.f;
        for (int i = 0; i < kBins; ++i)
        {
            const float c = center(i);
            hist[i] = std::exp(-0.5f * ((c + 9.f) / 3.f) * ((c + 9.f) / 3.f));
            total += hist[i];
        }
        const float scale = 12.f / total;
        total = 0.f;
        for (int i = 0; i < kBins; ++i) { hist[i] *= scale; total += hist[i]; }
    }

    void updateVoice(float refDb, bool active, bool allowUpdate)
    {
        if (!active) return;
        if (!init) { voice = refDb; init = true; return; }
        if (!allowUpdate) return;
        voice += (refDb - voice) * (refDb > voice ? aUp : aDown);
    }

    float voiceDb(float fallback) const { return init ? voice : fallback; }

    void updateRatio(float r, float s)
    {
        if (total > 4.f)
        {
            for (int i = 0; i < kBins; ++i) hist[i] *= decay;
            total *= decay;
        }
        if (s > 0.6f)
        {
            const int b = std::max(0, std::min(kBins - 1, (int) ((r - kLo) / step())));
            hist[b] += 1.f;
            total += 1.f;
        }
    }

    float percentile(float p) const
    {
        const float target = p * total;
        float cum = 0.f;
        for (int i = 0; i < kBins; ++i)
        {
            cum += hist[i];
            if (cum >= target) return center(i);
        }
        return center(kBins - 1);
    }

    // sens: 0..1 (1 = more sensitive). Returns threshold in ratio dB.
    // Baseline threshold comes from the control; the adaptive part only NUDGES it (max +-4 dB)
    // according to how hot this singer's sibilance is, so the plugin always reduces prominent
    // "s" sounds instead of drifting up until nothing is processed.
    float threshold(float sens, bool adaptive) const
    {
        const float base = lerpf(8.f, -16.f, sens);   // spans the typical sibilance ratios over the full travel
        if (!adaptive) return base;
        const float off = clampf(0.35f * percentile(0.5f), -4.f, 4.f);
        return clampf(base + off, -20.f, 12.f);
    }

private:
    static float step() { return (kHi - kLo) / (float) kBins; }
    static float center(int i) { return kLo + (i + 0.5f) * step(); }

    float hist[kBins] {};
    float total = 0.f, voice = -200.f, aUp = 0.01f, aDown = 0.001f, decay = 0.999f;
    bool init = false;
};

} // namespace naturalair
