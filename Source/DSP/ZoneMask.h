#pragma once
#include <vector>
#include "DspUtils.h"

namespace naturalair {

struct AirKeepParams
{
    float harshDepth, airReduction, airLiftFrac, sigmaOct, airStartOct, guardTolDb;
};

// Air Keep 0..1 drives three behaviours at once: air protection/lift,
// harsh-zone geometry and the air-ratio guard.
inline AirKeepParams mapAirKeep(float a)
{
    a = clampf(a, 0.f, 1.f);
    const float protect = smoothstep(0.f, 0.40f, a);   // air reduction is exactly 0 from Air Keep 40 % up
    const float lift = smoothstep(0.6f, 1.f, a);
    return { 1.f,
             1.f - protect,
             0.30f * lift,
             lerpf(0.55f, 0.32f, a),   // (engine sets the final sigma from Width, see setParams)
             lerpf(0.60f, 0.35f, a),
             lerpf(12.f, 3.f, a) };
}

// Builds per-bin gain curves in log-frequency: gaussian "harsh" zone around fc
// and a sigmoid "air" zone above it (no crossover filters, phase untouched).
class ZoneMask
{
public:
    void prepare(float sampleRate, int fftSize)
    {
        nBins = fftSize / 2 + 1;
        const float binHz = sampleRate / (float) fftSize;
        lf.assign((size_t) nBins, -10.f);
        taper.assign((size_t) nBins, 1.f);
        const float fHi = std::min(18000.f, 0.46f * sampleRate);
        kMin = std::max(1, (int) std::ceil(1000.f / binHz));
        for (int k = 1; k < nBins; ++k)
        {
            const float f = (float) k * binHz;
            lf[(size_t) k] = std::log2(f);
            taper[(size_t) k] = 1.f - smoothstep(0.85f * fHi, fHi, f);
        }
    }

    // Linear REDUCTION mask 0..1 (1 = this bin gets the full reduction R). Built from the smooth zone
    // blended with the exact spectral shape of the sibilant (spec), plus the air-zone reduction that
    // Air Keep leaves at low settings. Frequency-smoothed over 3 bins (short impulse response).
    void buildMask(float* m, float fc, const AirKeepParams& ak,
                   const float* spec = nullptr, float precision = 0.f) const
    {
        const float lfc = std::log2(fc), lAir = lfc + ak.airStartOct;
        const float specW = precisionToSpecWeight(precision);
        const float lLow = std::log2(std::max(kLowFloorHz, kLowFrac * fc));   // body / presence stay untouched below this
        for (int k = 0; k < nBins; ++k)
        {
            if (k < kMin) { m[k] = 0.f; continue; }
            const float l = lf[(size_t) k];
            const float wAir = 1.f / (1.f + std::exp(-(l - lAir) / kAirWidth));
            const float d = (l - lfc) / ak.sigmaOct;
            const float wLow = 1.f / (1.f + std::exp(-(l - lLow) / kLowWidth));
            float wH = std::exp(-0.5f * d * d) * (1.f - wAir) * wLow;
            if (spec != nullptr && precision > 0.f)
                wH = (1.f - specW) * wH + specW * spec[k] * (1.f - wAir) * wLow;
            wH *= airTail(l, ak);
            // kCoreBoost: the zone/spectral weight only reaches ~0.4-0.9 inside the sibilant, and the bin gain is
            // 1-(1-g)*m, so with m<1 the cut saturated far below Range (Range 16 -> 40 changed almost nothing).
            // Saturating the core to 1 lets the real depth follow the Range knob; the edges still taper.
            const float w = std::min(1.f, wH * ak.harshDepth * kCoreBoost + ak.airReduction * wAir);
            // y = x - (1-g)*S mixes in amplitude, so a mild expansion keeps the dB depth roughly proportional to the
            // weight. It used to be 2w-w^2, which lifted the flanks (w=.3 -> .51) and bled into the presence band;
            // w*(1.4-.4w) keeps the core at 1 but leaves the flanks close to their real weight (w=.3 -> .35).
            m[k] = w * (1.4f - 0.4f * w);
        }
        float prev = m[kMin];
        for (int k = kMin + 1; k < nBins - 1; ++k)
        {
            const float cur = m[k];
            m[k] = 0.25f * prev + 0.5f * cur + 0.25f * m[k + 1];
            prev = cur;
        }
    }

    // Linear band-pass mask used by Listen > Detector.
    void buildDetector(float* lin, float fc, const AirKeepParams& ak,
                       const float* spec = nullptr, float precision = 0.f) const
    {
        const float lfc = std::log2(fc), lAir = lfc + ak.airStartOct;
        const float specW = precisionToSpecWeight(precision);
        const float lLow = std::log2(std::max(kLowFloorHz, kLowFrac * fc));
        for (int k = 0; k < nBins; ++k)
        {
            if (k < kMin) { lin[k] = 0.f; continue; }
            const float l = lf[(size_t) k];
            const float wAir = 1.f / (1.f + std::exp(-(l - lAir) / kAirWidth));
            const float d = (l - lfc) / ak.sigmaOct;
            const float wLow = 1.f / (1.f + std::exp(-(l - lLow) / kLowWidth));
            float w = std::exp(-0.5f * d * d) * (1.f - wAir) * wLow;
            if (spec != nullptr && precision > 0.f)
                w = (1.f - specW) * w + specW * spec[k] * (1.f - wAir) * wLow;
            w *= airTail(l, ak);
            lin[k] = w;
        }
    }

private:
    // Precision 0..1 -> weight of the exact spectral shape. From ~85 % up the wide bell is essentially gone
    // (mask = spec mask), below that it fades in like before.
    static float precisionToSpecWeight(float p) { return clampf(p + (1.f - p) * smoothstep(0.5f, 0.85f, p), 0.f, 1.f); }
    // The hard zone never reaches the "air" region (>~10.5-12.5 kHz) unless Air Keep is low (airReduction > 0).
    static float airTail(float logHz, const AirKeepParams& ak)
    {
        const float f = std::exp2(logHz);
        return 1.f - (1.f - ak.airReduction) * smoothstep(10500.f, 12500.f, f);
    }
    static constexpr float kAirWidth = 0.2f; // octaves
    static constexpr float kCoreBoost = 1.7f;   // saturates the core to 1 without widening the zone much
    static constexpr float kLowFloorHz = 4000.f, kLowFrac = 0.65f, kLowWidth = 0.06f;
    int nBins = 257, kMin = 11;
    std::vector<float> lf, taper;
};

} // namespace naturalair
