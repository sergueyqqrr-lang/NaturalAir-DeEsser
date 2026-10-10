#pragma once
#include <vector>
#include "DspUtils.h"

namespace naturalair {

struct FrameFeatures
{
    float refDb = -200.f, sibDb = -200.f, airDb = -200.f, lowDb = -200.f;
    float r = -60.f;          // sibilance/body ratio: max(mean PSD, strongest sub-band - kPeakBiasDb), dB
    float rMean = -60.f;      // ratio of the plain mean over 3.5-11 kHz
    float rPeak = -60.f;      // ratio of the strongest ~1.2 kHz sub-band (a narrow "s" is not diluted by the mean)
    float sibPeakDb = -200.f; // absolute level of that strongest sub-band, dB
    float rGain = -60.f;      // ratio used by the REDUCTION law (same peak/sub-band criterion as the detector, lighter bias)
    float sfm = 0.f;          // spectral flatness 3.5-11 kHz
    float centroid = 7000.f;  // Hz
    float peakHz = 7000.f;    // frequency of the strongest sibilance peak (smoothed spectrum)
    float lowFluxDb = 0.f;    // low-band rise vs previous frame
    bool  active = false;     // voice present
    float s = 0.f;            // sibilance probability 0..1
    float Rraw = 0.f;         // target reduction (dB) before confirmation
};

class FeatureExtractor
{
public:
    void prepare(float sampleRate, int fftSize)
    {
        N = fftSize;
        nBins = N / 2 + 1;
        binHz = sampleRate / (float) N;
        auto bin = [&](float f) { return std::max(1, std::min(nBins - 2, (int) std::lround(f / binHz))); };
        kRef0 = bin(500.f);  kRef1 = std::max(kRef0 + 1, bin(3000.f));
        kSib0 = bin(3500.f); kSib1 = std::max(kSib0 + 2, bin(std::min(11000.f, 0.46f * sampleRate)));
        kAir0 = bin(std::min(10000.f, 0.40f * sampleRate)); kAir1 = std::max(kAir0 + 1, bin(0.5f * sampleRate * 0.95f));
        kLow0 = bin(80.f);   kLow1 = std::max(kLow0 + 1, bin(500.f));
        prevLowDb = -200.f; first = true;
    }

    // P: normalised power spectrum (nBins), summed over channels.
    void compute(const float* P, FrameFeatures& o)
    {
        auto mean = [&](int a, int b)
        {
            double s = 0.0;
            for (int k = a; k <= b; ++k) s += P[k];
            return (float) (s / (b - a + 1));
        };
        o.refDb = powToDb(mean(kRef0, kRef1));
        o.sibDb = powToDb(mean(kSib0, kSib1));
        o.airDb = powToDb(mean(kAir0, kAir1));
        o.lowDb = powToDb(mean(kLow0, kLow1));
        o.rMean = o.sibDb - o.refDb;
        // peak sub-band: sliding mean over ~1.2 kHz inside the sibilance range. A narrow "s" around 7 kHz fills only
        // part of 3.5-11 kHz, so the plain mean under-reports it; the flat-noise bias (kPeakBiasDb) keeps hiss from gaining.
        {
            const int w = std::max(2, (int) std::lround(1200.f / binHz));
            double acc = 0.0; float best = 0.f;
            for (int k = kSib0; k <= kSib1; ++k)
            {
                acc += P[k];
                if (k - kSib0 >= w) acc -= P[k - w];
                if (k - kSib0 >= w - 1) best = std::max(best, (float) (acc / w));
            }
            o.sibPeakDb = powToDb(best);
            o.rPeak = o.sibPeakDb - o.refDb;
        }
        o.r = std::max(o.rMean, o.rPeak - kPeakBiasDb);
        // Ratio that feeds the gain law. The detector (score) already accepts the strongest sub-band as evidence of an "s";
        // if the reduction only looked at the diluted mean (or at the peak with the full flat-noise bias) a short/moderate
        // "s" scored high and then fell below the threshold. The score still gates the result, so hiss cannot gain from the
        // lighter bias (kPeakBiasGainDb < kPeakBiasDb).
        o.rGain = std::max(o.rMean, o.rPeak - kPeakBiasGainDb);

        double sumLog = 0.0, sumP = 0.0, num = 0.0;
        for (int k = kSib0; k <= kSib1; ++k)
        {
            const double p = (double) P[k] + 1e-30;
            sumLog += std::log(p); sumP += p; num += p * (double) k * (double) binHz;
        }
        const int n = kSib1 - kSib0 + 1;
        o.sfm = (float) (std::exp(sumLog / n) / (sumP / n));
        o.centroid = sumP > 0.0 ? (float) (num / sumP) : 7000.f;
        float best = -1.f; int kBest = kSib0;
        for (int k = kSib0; k <= kSib1; ++k)
        {
            const float v = P[k - 1] + 2.f * P[k] + P[k + 1];
            if (v > best) { best = v; kBest = k; }
        }
        o.peakHz = (float) kBest * binHz;

        o.lowFluxDb = first ? 0.f : o.lowDb - prevLowDb;
        prevLowDb = o.lowDb; first = false;
        o.active = o.refDb > kGateDb;
    }

    static constexpr float kGateDb = -80.f;
    static constexpr float kPeakBiasDb = 6.f;   // expected excess of a sub-band max over the mean for flat noise (detector)
    static constexpr float kPeakBiasGainDb = 4.f;   // same, for the reduction ratio (score already gates it)
    static constexpr float kPeakBiasLevelDb = 5.f;  // same, for the level cue (rel) of the score

private:
    int N = 512, nBins = 257;
    int kRef0 = 5, kRef1 = 32, kSib0 = 37, kSib1 = 117, kAir0 = 106, kAir1 = 220, kLow0 = 1, kLow1 = 5;
    float binHz = 93.75f, prevLowDb = -200.f;
    bool first = true;
};

// Soft-AND of independent cues: every condition must hold to some degree.
struct SibilanceDetector
{
    static float score(const FrameFeatures& f, float voiceDb)
    {
        if (!f.active) return 0.f;
        // Level of the sibilance vs. the vowel: the mean over 3.5-11 kHz dilutes a narrow "s", so the strongest sub-band
        // (minus a small noise bias) counts too. Same peak criterion as rPeak, so sR and sL no longer disagree.
        const float rel = std::max(f.sibDb, f.sibPeakDb - FeatureExtractor::kPeakBiasLevelDb) - voiceDb;
        // Calibrated on labelled speech (see Tools/process_wav): real "s" sounds have a peaky
        // spectrum (flatness ~0.15-0.25) and a level that varies a lot against the vowel
        // reference, so the flatness cue stays loose; the level cue is what rejects breaths and room noise.
        const float sR = ramp(f.r, -14.f, -4.f);
        // Flatness cue: peaky (low sfm) "s" must not be punished, so the range starts lower and the cue can only
        // pull the score down to 35 % (it used to be able to zero it).
        const float sS = 0.35f + 0.65f * ramp(f.sfm, 0.015f, 0.06f);
        const float sL = ramp(rel, -32.f, -22.f);   // breaths/room noise sit ~-34 dB or lower
        const float plosive = 0.9f * ramp(f.lowFluxDb, 3.f, 9.f);
        // exponents sum to 1 (geometric mean): one strong cue is no longer dragged down by a mediocre one
        return std::pow(sR, 0.45f) * std::pow(sS, 0.15f) * std::pow(sL, 0.40f) * (1.f - plosive);
    }
};

} // namespace naturalair
