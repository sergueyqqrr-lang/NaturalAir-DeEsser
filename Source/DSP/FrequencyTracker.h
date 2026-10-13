#pragma once
#include "DspUtils.h"

namespace naturalair {

// Auto-frequency: locks the sibilance centre during each event (no wobble
// inside an "s"), drifts slowly toward the singer's historical centre between events.
class FrequencyTracker
{
public:
    void prepare(float frameRateHz, float sampleRate)
    {
        fMax = std::min(15000.f, 0.45f * sampleRate);
        aDrift = 1.f - std::exp(-1.f / (1.0f * frameRateHz));
        lockFrames = std::max(3, (int) std::lround(0.012f * frameRateHz));   // ~12 ms window over which the centre keeps refining
        reset();
    }

    void reset()
    {
        fcLong = locked = current = 7000.f;
        inEvent = false; n = 0; acc = w = 0.f;
    }

    void update(float centroidHz, float peakHz, float s)
    {
        const float centroid = 0.7f * peakHz + 0.3f * centroidHz;   // peak = exact, centroid = stable
        const bool on = inEvent ? (s > 0.35f) : (s > 0.5f);
        if (on)
        {
            if (!inEvent) { inEvent = true; n = 0; acc = w = 0.f; }
            if (n < lockFrames)
            {
                // weight ~ s^2: the clearest frames of the event dominate, the half-filled frames at the onset (noisy peaks) do not
                const float wt = s * s;
                acc += centroid * wt; w += wt; ++n;
                locked = clampf(acc / std::max(w, 1e-6f), 3500.f, fMax);
            }
            current = locked;
        }
        else
        {
            if (inEvent) { inEvent = false; fcLong += (locked - fcLong) * 0.25f; }
            current += (fcLong - current) * aDrift;
        }
    }

    float frequency() const { return current; }
    bool  inEventNow() const { return inEvent; }

private:
    float fcLong = 7000.f, locked = 7000.f, current = 7000.f, fMax = 12000.f, aDrift = 0.01f;
    float acc = 0.f, w = 0.f;
    int n = 0, lockFrames = 6;
    bool inEvent = false;
};

} // namespace naturalair
