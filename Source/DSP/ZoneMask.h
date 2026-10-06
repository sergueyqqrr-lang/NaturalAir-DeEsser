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
    const float protect = smoothstep(0.f, 0.6f, a);
    const float lift = smoothstep(0.6f, 1.f, a);
    return { 1.f,
             1.f - protect,
             0.30f * lift,
             lerpf(0.55f, 0.32f, a),
             lerpf(0.90f, 0.55f, a),
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
        const float lLow = std::log2(std::max(2000.f, 0.45f * fc));   // never touch the vowel body / presence below this
        for (int k = 0; k < nBins; ++k)
        {
            if (k < kMin) { m[k] = 0.f; continue; }
            const float l = lf[(size_t) k];
            const float wAir = 1.f / (1.f + std::exp(-(l - lAir) / kAirWidth));
            const float d = (l - lfc) / ak.sigmaOct;
            const float wLow = 1.f / (1.f + std::exp(-(l - lLow) / 0.12f));
            float wH = std::exp(-0.5f * d * d) * (1.f - wAir) * wLow;
            if (spec != nullptr && precision > 0.f)
                wH = (1.f - precision) * wH + precision * spec[k] * (1.f - wAir) * wLow;
            // kCoreBoost: the zone/spectral weight only reaches ~0.4-0.9 inside the sibilant, and the bin gain is
            // 1-(1-g)*m, so with m<1 the cut saturated far below Range (Range 16 -> 40 changed almost nothing).
            // Saturating the core to 1 lets the real depth follow the Range knob; the edges still taper.
            const float w = std::min(1.f, wH * ak.harshDepth * kCoreBoost + ak.airReduction * wAir);
            m[k] = 2.f * w - w * w;   // y = x - (1-g)*S mixes in amplitude; this expands the mask so the dB depth
                                      // matches the old dB-linear behaviour (m=.5 -> half the dB reduction)
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
        const float lLow = std::log2(std::max(2000.f, 0.45f * fc));
        for (int k = 0; k < nBins; ++k)
        {
            if (k < kMin) { lin[k] = 0.f; continue; }
            const float l = lf[(size_t) k];
            const float wAir = 1.f / (1.f + std::exp(-(l - lAir) / kAirWidth));
            const float d = (l - lfc) / ak.sigmaOct;
            const float wLow = 1.f / (1.f + std::exp(-(l - lLow) / 0.12f));
            float w = std::exp(-0.5f * d * d) * (1.f - wAir) * wLow;
            if (spec != nullptr && precision > 0.f)
                w = (1.f - precision) * w + precision * spec[k] * (1.f - wAir) * wLow;
            lin[k] = w;
        }
    }

private:
    static constexpr float kAirWidth = 0.2f; // octaves
    static constexpr float kCoreBoost = 3.0f;
    int nBins = 257, kMin = 11;
    std::vector<float> lf, taper;
};

} // namespace naturalair
