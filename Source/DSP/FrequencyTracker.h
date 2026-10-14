#pragma once
#include "DspUtils.h"

namespace naturalair {

// Auto-frequency: follows the centre of the CURRENT "s".
//  - the estimate is the local peak of the short spectrum (the broad-band centroid only matters while the evidence is weak);
//  - an event starts easily (short / weak "s"), the first frames give a quick weighted average (no wobble from the noisy onset),
//    and after that the centre keeps FOLLOWING the peak (soft lock): slowly while it is stable, immediately when the peak
//    moves consistently (a different colour, an "s" -> "sh" glide);
//  - between events it drifts back towards the singer's historical centre, which only learns from clear events.
class FrequencyTracker
{
public:
    void prepare(float frameRateHz, float sampleRate)
    {
        fMax = std::min(15000.f, 0.45f * sampleRate);
        aDrift = 1.f - std::exp(-1.f / (0.5f * frameRateHz));
        aFollow = 1.f - std::exp(-1.f / (0.004f * frameRateHz));     // ~4 ms: follows the peak inside an event
        lockFrames = std::max(2, (int) std::lround(0.004f * frameRateHz));   // ~4 ms: quick weighted average at the onset
        reset();
    }

    void reset()
    {
        fcLong = locked = current = 7000.f;
        inEvent = false; n = 0; acc = w = 0.f; evMax = 0.f; off = 0; mis = 0;
    }

    void update(float centroidHz, float peakHz, float s)
    {
        // peak weight 0.8 -> 1.0 as the "s" becomes clear: the broad-band centroid no longer pulls the centre away from the real peak
        const float wPeak = lerpf(0.8f, 1.0f, ramp(s, 0.35f, 0.70f));
        const float lEst = wPeak * std::log2(std::max(peakHz, 1000.f)) + (1.f - wPeak) * std::log2(std::max(centroidHz, 1000.f));
        const float est = clampf(std::exp2(lEst), 3500.f, fMax);
        const bool on = inEvent ? (s > 0.18f) : (s > 0.30f);
        if (on)
        {
            if (!inEvent) { inEvent = true; n = 0; acc = w = 0.f; evMax = 0.f; off = 0; mis = 0; }
            evMax = std::max(evMax, s);
            const float wt = std::max(s * s, 0.02f);
            if (n < lockFrames)
            {
                // onset: weighted average (the clearest frames dominate), the first frame is already the estimate
                acc += est * wt; w += wt; ++n;
                locked = clampf(acc / std::max(w, 1e-6f), 3500.f, fMax);
            }
            else
            {
                // soft lock: follow the peak. A consistent jump (>0.2 octave for 2 frames) re-locks at once.
                const float dev = std::fabs(std::log2(est / locked));
                mis = dev > 0.2f ? mis + 1 : 0;
                const float a = mis >= 2 ? 0.8f : aFollow * ramp(s, 0.15f, 0.6f);
                locked = clampf(locked + (est - locked) * a, 3500.f, fMax);
                if (mis >= 2) { acc = est * wt; w = wt; mis = 0; }
            }
            current = locked;
            off = 0;
        }
        else
        {
            // 2 frames of grace so a one-frame dip of the score does not end the event
            if (inEvent && ++off >= 2)
            {
                inEvent = false;
                if (evMax > 0.5f) fcLong += (locked - fcLong) * 0.5f;   // only clear "s" teach the history
            }
            if (!inEvent) current += (fcLong - current) * aDrift;
        }
    }

    float frequency() const { return current; }
    bool  inEventNow() const { return inEvent; }

private:
    float fcLong = 7000.f, locked = 7000.f, current = 7000.f, fMax = 12000.f, aDrift = 0.01f, aFollow = 0.2f;
    float acc = 0.f, w = 0.f, evMax = 0.f;
    int n = 0, lockFrames = 3, off = 0, mis = 0;
    bool inEvent = false;
};

} // namespace naturalair
