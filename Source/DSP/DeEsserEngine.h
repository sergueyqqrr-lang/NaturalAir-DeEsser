#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "AdaptiveThreshold.h"
#include "DspUtils.h"
#include "EnvelopeSmoother.h"
#include "Fft.h"
#include "FrequencyTracker.h"
#include "GainComputer.h"
#include "SibilanceDetector.h"
#include "ZoneMask.h"

namespace naturalair {

struct Params
{
    float airKeep = 0.65f;    // 0..1
    float threshold = 0.5f;   // 0..1 sensitivity (1 = more sensitive)
    float rangeDb = 12.f;     // max reduction at the centre of the sibilant (0..40)
    float precision = 0.75f;  // 0 = smooth zone, 1 = only the exact frequencies where the "s" has energy
    float widthOct = 0.40f;   // width (sigma, octaves) of the smooth zone (0.1..2)
    float freqHz = 7000.f;    // manual centre
    bool  freqAuto = true;
    float attackMs = 0.5f;    // 0.01 .. 200 (engine accepts up to 500)
    float releaseMs = 50.f;   // 0.5 .. 3000 (engine accepts up to 5000)
    float airBoostDb = 0.f;   // 0..3
    float mix = 1.f;          // 0..1
    float inputDb = 0.f, outputDb = 0.f;
    int   listen = 0;         // 0 off, 1 delta, 2 detector band
    bool  adaptive = true;
    bool  bypass = false;
};

// Two-path dynamic spectral de-esser.
//
//  FAST path (short STFT, N_S = 256 @ 48 kHz, ~5 ms): decides WHEN and HOW MUCH. Sibilance detection,
//    adaptive threshold, frequency tracking and the attack/release envelope R(t). 4 frames of lookahead.
//  FINE path (long STFT, N_L = 4 x N_S = 1024 @ 48 kHz, 47 Hz bins): decides WHERE. It finds the exact
//    frequencies where this sibilant has energy and extracts that component S(t) of the signal.
//  Output:  y = x - (1 - g(t)) * S(t) + (gUp(t) - 1) * HP(x),  g(t) = 10^(-R(t)/20) at sample rate.
//  The reduction only ever touches the selected component, so everything else stays bit-identical
//  (R = 0  ->  y == x exactly). Latency = N_L samples (fixed per sample rate; 1024 = 21.3 ms @ 48 kHz).
class DeEsserEngine
{
public:
    static constexpr int kMaxChannels = 2;
    static constexpr int kLookahead = 4;
    static constexpr int kHist = kLookahead + 1;
    static constexpr int kMeterPoints = 96;
    static constexpr int kRing = 128;            // per-short-frame control ring (power of two)

    void prepare(double sampleRate, int numChannels)
    {
        sr = (float) sampleRate;
        nCh = std::max(1, std::min(kMaxChannels, numChannels));
        nS = sr <= 50000.f ? 256 : (sr <= 100000.f ? 512 : 1024);
        hS = nS / 4;
        nBinsS = nS / 2 + 1;
        nL = 4 * nS;
        hL = nL / 4;
        nBinsL = nL / 2 + 1;
        L = nL;
        fftS.init(nS);
        fftL.init(nL);

        windowS.resize((size_t) nS);
        for (int i = 0; i < nS; ++i) windowS[(size_t) i] = std::sqrt(0.5f - 0.5f * (float) std::cos(2.0 * kPi * i / nS));
        windowL.resize((size_t) nL);
        for (int i = 0; i < nL; ++i) windowL[(size_t) i] = std::sqrt(0.5f - 0.5f * (float) std::cos(2.0 * kPi * i / nL));

        inMask = (uint64_t) nextPow2(2 * nL) - 1;
        outMask = (uint64_t) nextPow2(4 * nL) - 1;
        for (int c = 0; c < nCh; ++c)
        {
            auto& ch = chan[(size_t) c];
            ch.inRing.assign((size_t) inMask + 1, 0.f);
            ch.sRing.assign((size_t) outMask + 1, 0.f);
            ch.dryG.assign((size_t) outMask + 1, 0.f);
            ch.dryR.assign((size_t) outMask + 1, 0.f);
            ch.re.assign((size_t) nBinsL, 0.f);
            ch.im.assign((size_t) nBinsL, 0.f);
            ch.hp.setHighpass(sr, std::min(9500.f, 0.42f * sr), 0.707f);
        }
        workRe.assign((size_t) nL, 0.f);
        workIm.assign((size_t) nL, 0.f);
        PS.assign((size_t) nBinsS, 0.f);
        PL.assign((size_t) nBinsL, 0.f);
        maskCurve.assign((size_t) nBinsL, 0.f);
        for (auto* v : { &psDb, &maskRaw, &maskPre, &specMask }) v->assign((size_t) nBinsL + 1, 0.f);

        const float binHzL = sr / (float) nL;
        kSpecLo = std::max(2, (int) std::lround(4000.f / binHzL));
        kSpecHi = std::min(nBinsL - 3, (int) std::lround(std::min(16000.f, 0.46f * sr) / binHzL));
        aMask = 1.f - std::exp(-(float) hL / (0.004f * sr));
        aMaskFall = 1.f - std::exp(-(float) hL / (kMaskHoldMs * 0.001f * sr));

        frameRate = sr / (float) hS;
        normPS = 1.f / ((float) nS * (float) nS);
        normPL = 1.f / ((float) nL * (float) nL);
        extractor.prepare(sr, nS);
        zone.prepare(sr, nL);
        adapt.prepare(frameRate);
        tracker.prepare(frameRate, sr);
        env.prepare(frameRate);

        const float fTop = std::min(20000.f, 0.5f * sr * 0.98f);
        for (int p = 0; p < kMeterPoints; ++p)
        {
            const float f = 500.f * std::pow(fTop / 500.f, (float) p / (float) (kMeterPoints - 1));
            meterBin[(size_t) p] = std::max(1, std::min(nBinsL - 1, (int) std::lround(f / binHzL)));
        }

        for (auto* s : { &inGainS, &outGainS, &mixS, &bypassS, &deltaS }) s->setTime(0.010f, sr);
        bypassS.setTime(0.008f, sr);
        aFc = 1.f - std::exp(-1.f / (0.003f * frameRate));
        airBoostS.setTime(0.05f, frameRate);
        activityS.setTime(0.15f, frameRate);
        presenceS.setTime(0.08f, frameRate);
        gateS.setTime(0.006f, frameRate);

        setParams(Params {});
        reset();
        prepared = true;
    }

    void reset()
    {
        nIn = 0;
        for (int c = 0; c < nCh; ++c)
        {
            auto& ch = chan[(size_t) c];
            std::fill(ch.inRing.begin(), ch.inRing.end(), 0.f);
            std::fill(ch.sRing.begin(), ch.sRing.end(), 0.f);
            std::fill(ch.dryG.begin(), ch.dryG.end(), 0.f);
            std::fill(ch.dryR.begin(), ch.dryR.end(), 0.f);
            ch.hp.reset();
        }
        for (auto& f : feats) f = FrameFeatures {};
        holdVal = 0.f; holdCnt = 0; ringR.fill(0.f); ringUp.fill(0.f); ringGate.fill(0.f); ringFc.fill(7000.f);
        adapt.reset(); tracker.reset(); env.reset();
        extractor.prepare(sr, nS);
        lfcS = std::log2(7000.f);
        std::fill(specMask.begin(), specMask.end(), 0.f);
        airBoostS.snap(0.f); activityS.snap(0.f); presenceS.snap(0.f); gateS.snap(0.f);
        inGainS.snap(inGainT); outGainS.snap(outGainT); mixS.snap(mixT);
        bypassS.snap(bypassT); deltaS.snap(deltaT);
        reductionDb.store(0.f);
    }

    int getLatencySamples() const { return L; }
    bool isPrepared() const { return prepared; }

    void setParams(const Params& p)
    {
        params = p;
        params.airKeep = clampf(p.airKeep, 0.f, 1.f);
        params.threshold = clampf(p.threshold, 0.f, 1.f);
        params.rangeDb = clampf(p.rangeDb, 0.f, 60.f);
        params.precision = clampf(p.precision, 0.f, 1.f);
        ak = mapAirKeep(params.airKeep);
        // Width is the real control of the zone, and Air Keep narrows it further (it used to be overwritten).
        widthN = clampf((p.widthOct - 0.10f) / 0.60f, 0.f, 1.f);                       // 0..1 over the 0.10..0.70 oct range
        ak.sigmaOct = clampf(p.widthOct, 0.10f, 0.70f) * lerpf(1.f, 0.65f, params.airKeep);
        guardMaxR = ak.guardTolDb / std::max(ak.airReduction * 0.85f, 1e-3f);
        params.attackMs = clampf(p.attackMs, 0.01f, 500.f);
        params.releaseMs = clampf(p.releaseMs, 0.5f, 5000.f);
        if (std::fabs(params.attackMs - lastAttack) > 1e-6f || std::fabs(params.releaseMs - lastRelease) > 1e-6f)
        {
            env.setTimes(params.attackMs, params.releaseMs);
            lastAttack = params.attackMs; lastRelease = params.releaseMs;
        }
        inGainT = dbToLin(p.inputDb);
        outGainT = dbToLin(p.outputDb);
        mixT = clampf(p.mix, 0.f, 1.f);
        bypassT = p.bypass ? 1.f : 0.f;
        deltaT = p.listen == 1 ? 1.f : 0.f;
    }

    // In-place processing. io[c] has numSamples floats, c < numChannels given to prepare().
    void process(float* const* io, int numSamples)
    {
        if (!prepared) return;
        float peakIn = 0.f, peakOut = 0.f;
        for (int i = 0; i < numSamples; ++i)
        {
            const float inG = inGainS.process(inGainT);
            const float outG = outGainS.process(outGainT);
            const float mix = mixS.process(mixT);
            const float byp = bypassS.process(bypassT);
            const float dlt = deltaS.process(deltaT);

            const uint64_t idx = nIn;
            for (int c = 0; c < nCh; ++c)
            {
                float x = io[c][i];
                if (!std::isfinite(x)) x = 0.f;
                peakIn = std::max(peakIn, std::fabs(x));
                auto& ch = chan[(size_t) c];
                ch.inRing[idx & inMask] = x * inG;
                ch.dryR[idx & outMask] = x;
                ch.dryG[idx & outMask] = x * inG;
            }
            ++nIn;
            if (nIn >= (uint64_t) nS && (nIn - (uint64_t) nS) % (uint64_t) hS == 0)
                processShortFrame((nIn - (uint64_t) nS) / (uint64_t) hS);
            if (nIn >= (uint64_t) nL && (nIn - (uint64_t) nL) % (uint64_t) hL == 0)
                processLongFrame((nIn - (uint64_t) nL) / (uint64_t) hL);

            // control signals at sample rate (linear interpolation between short-frame centres)
            const int64_t m = (int64_t) idx - (int64_t) L;
            float R = 0.f, up = 0.f, gate = 0.f;
            if (m >= 0)
            {
                const int64_t u = m - (int64_t) (nS / 2);
                const int64_t gi = u >= 0 ? u / hS : 0;
                const float fr = u >= 0 ? (float) (u % hS) / (float) hS : 0.f;
                const size_t i0 = (size_t) ((uint64_t) gi & (uint64_t) (kRing - 1));
                const size_t i1 = (size_t) ((uint64_t) (gi + 1) & (uint64_t) (kRing - 1));
                R = ringR[i0] + (ringR[i1] - ringR[i0]) * fr;
                up = ringUp[i0] + (ringUp[i1] - ringUp[i0]) * fr;
                gate = ringGate[i0] + (ringGate[i1] - ringGate[i0]) * fr;
            }
            // Sibilance gate on the reduction: fully open while an "s" is detected (score >= ~0.1, so the depth on the "s"
            // is unchanged) and closing within ~10 ms when it ends. Without it the release tail kept subtracting S
            // from the vowel that follows, and S then holds that vowel's own 5-9 kHz brightness.
            const float gateOpen = smoothstep(0.f, 0.3f, gate);
            const float oneMinusG = R > 1.0e-4f ? (1.f - std::exp(-R * 0.11512925f)) * gateOpen : 0.f;
            const float gUpM1 = up > 1.0e-4f ? std::exp(up * 0.11512925f) - 1.f : 0.f;

            for (int c = 0; c < nCh; ++c)
            {
                auto& ch = chan[(size_t) c];
                float wet = 0.f, dryG = 0.f, dryR = 0.f;
                if (m >= 0)
                {
                    const size_t j = (size_t) ((uint64_t) m & outMask);
                    const float S = ch.sRing[j]; ch.sRing[j] = 0.f;
                    dryG = ch.dryG[j]; dryR = ch.dryR[j];
                    const float hp = ch.hp.process(dryG);
                    wet = params.listen == 2 ? S * gate : dryG - oneMinusG * S + gUpM1 * hp;
                }
                const float normal = dryG * (1.f - mix) + wet * mix;
                float proc = params.listen == 2 ? wet : normal + dlt * ((wet - dryG) - normal);
                proc *= outG;
                float y = dryR * byp + proc * (1.f - byp);
                if (!std::isfinite(y)) y = 0.f;
                peakOut = std::max(peakOut, std::fabs(y));
                io[c][i] = y;
            }
        }
        peakInM.store(std::max(peakInM.load(std::memory_order_relaxed), peakIn), std::memory_order_relaxed);
        peakOutM.store(std::max(peakOutM.load(std::memory_order_relaxed), peakOut), std::memory_order_relaxed);
    }

    // ---- meters (safe to read from the UI thread) ----
    float getReductionDb() const { return reductionDb.load(std::memory_order_relaxed); }
    float getFrequencyHz() const { return fcMeter.load(std::memory_order_relaxed); }
    // detector internals (newest frame) - handy for calibration/debugging
    float getDebugRatioDb() const { return dbgRatio.load(std::memory_order_relaxed); }
    float getDebugScore() const { return dbgScore.load(std::memory_order_relaxed); }
    float getDebugSfm() const { return dbgSfm.load(std::memory_order_relaxed); }
    float getDebugRelDb() const { return dbgRel.load(std::memory_order_relaxed); }
    float getDebugThresholdDb() const { return dbgThr.load(std::memory_order_relaxed); }
    float getDebugRawReductionDb() const { return dbgRaw.load(std::memory_order_relaxed); }   // target before confirm/envelope
    float consumeInputPeak() { return peakInM.exchange(0.f, std::memory_order_relaxed); }
    float consumeOutputPeak() { return peakOutM.exchange(0.f, std::memory_order_relaxed); }
    float getGainCurveDb(int p) const { return gainMeter[(size_t) p].load(std::memory_order_relaxed); }
    float getSpectrumDb(int p) const { return specMeter[(size_t) p].load(std::memory_order_relaxed); }
    float getMeterFrequency(int p) const { return (float) meterBin[(size_t) p] * sr / (float) nL; }
    float sampleRate() const { return sr; }
    // debug: reduction mask of the last long frame (0..1 per fine bin) and its bin width
    const std::vector<float>& debugMask() const { return maskCurve; }
    float debugBinHz() const { return sr / (float) nL; }

private:
    struct Biquad
    {
        float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f, z1 = 0.f, z2 = 0.f;
        void setHighpass(float fs, float fc, float q)
        {
            const float w = 2.f * (float) kPi * fc / fs, c = std::cos(w), al = std::sin(w) / (2.f * q), a0 = 1.f + al;
            b0 = 0.5f * (1.f + c) / a0; b1 = -(1.f + c) / a0; b2 = b0; a1 = -2.f * c / a0; a2 = (1.f - al) / a0;
        }
        void reset() { z1 = z2 = 0.f; }
        float process(float x) { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    };

    struct Chan
    {
        std::vector<float> inRing, sRing, dryG, dryR;   // sRing: overlap-added sibilant component S(t)
        std::vector<float> re, im;                       // long-window spectrum of the current long frame
        Biquad hp;                                       // air shelf component
    };

    // FAST path: detection, threshold, tracking, envelope -> control rings.
    void processShortFrame(uint64_t f)
    {
        const int slot = (int) (f % (uint64_t) kHist);
        const uint64_t start = f * (uint64_t) hS;

        std::fill(PS.begin(), PS.end(), 0.f);
        for (int c = 0; c < nCh; ++c)
        {
            auto& ch = chan[(size_t) c];
            for (int i = 0; i < nS; ++i)
            {
                workRe[(size_t) i] = ch.inRing[(start + (uint64_t) i) & inMask] * windowS[(size_t) i];
                workIm[(size_t) i] = 0.f;
            }
            fftS.forward(workRe.data(), workIm.data());
            for (int k = 0; k < nBinsS; ++k)
                PS[(size_t) k] += workRe[(size_t) k] * workRe[(size_t) k] + workIm[(size_t) k] * workIm[(size_t) k];
        }
        for (int k = 0; k < nBinsS; ++k) PS[(size_t) k] *= normPS;

        FrameFeatures& ft = feats[(size_t) slot];
        extractor.compute(PS.data(), ft);
        const float vdb = adapt.voiceDb(ft.refDb);
        ft.s = SibilanceDetector::score(ft, vdb);
        adapt.updateVoice(ft.refDb, ft.active, ft.s < 0.3f);
        // The histogram and the threshold live in the same ratio the gain law compares against (rGain).
        adapt.updateRatio(ft.rGain, ft.s);
        const float T = adapt.threshold(params.threshold, params.adaptive);
        // Score is only a gate now (0.15..0.55 -> 0..1) instead of a multiplier, so a clearly detected "s"
        // gets the full reduction instead of s * R (typically 50-70 % of it).
        const float sGate = ramp(ft.s, 0.20f, 0.50f);
        // Evidence: what the detector is sure about is passed on to the law (lower effective T, wider knee), so a high
        // score cannot end in R = 0 just because the ratio sits a few dB under the threshold.
        const float sEvidence = ramp(ft.s, 0.40f, 0.80f);
        ft.Rraw = sGate * GainComputer::reduction(ft.rGain, T, params.rangeDb, sEvidence);
        dbgRatio.store(ft.rGain, std::memory_order_relaxed);
        dbgRaw.store(ft.Rraw, std::memory_order_relaxed);
        dbgSfm.store(ft.sfm, std::memory_order_relaxed);
        dbgRel.store(ft.sibDb - vdb, std::memory_order_relaxed);
        dbgScore.store(ft.s, std::memory_order_relaxed);
        dbgThr.store(T, std::memory_order_relaxed);

        if (f < (uint64_t) kLookahead) return;

        // Frame g = f - K, decided with the evidence of the K frames that follow it (lookahead).
        const uint64_t g = f - (uint64_t) kLookahead;
        const int sg = (int) (g % (uint64_t) kHist);
        const FrameFeatures& fg = feats[(size_t) sg];
        int cnt = 0;
        float Rt = 0.f, evidence = 0.f;
        for (int j = 0; j <= kLookahead; ++j)
        {
            const FrameFeatures& fj = feats[(size_t) ((sg + j) % kHist)];
            if (fj.s > 0.35f) ++cnt;
            evidence = std::max(evidence, fj.s);
            Rt = std::max(Rt, (1.f - 0.1f * (float) j) * fj.Rraw);
        }
        // Confirmation: 2 frames with s > 0.35 OR one clearly strong frame. A short "s" lights only 1-2 frames of the
        // fast path, so waiting for a second one used to cut its depth; the lookahead only anticipates, it doesn't veto.
        const float strong = ramp(evidence, 0.6f, 0.9f);
        const float confirm = std::max(clampf((float) cnt / 2.f, 0.f, 1.f), strong);
        Rt *= 0.6f + 0.4f * confirm;
        // Minimum peak hold (2 frames, ~2.6 ms): an ultra-short "s" gets the peak for its whole length, not just its onset.
        // It does not extend the release: once the hold is over R follows the envelope as before.
        if (Rt >= holdVal) { holdVal = Rt; holdCnt = 2; }
        else if (holdCnt > 0) { --holdCnt; Rt = holdVal; }
        else holdVal = Rt;

        tracker.update(ft.centroid, ft.peakHz, ft.s);
        const float fcTarget = params.freqAuto ? tracker.frequency() : params.freqHz;
        lfcS += (std::log2(std::max(fcTarget, 1000.f)) - lfcS) * aFc;
        const float fc = std::exp2(lfcS);

        const float R = std::min(env.process(Rt, evidence), guardMaxR);
        reductionDb.store(R, std::memory_order_relaxed);
        fcMeter.store(fc, std::memory_order_relaxed);

        const float activity = activityS.process(fg.active ? 1.f : 0.f);
        const float presence = presenceS.process(ramp(fg.airDb - fg.refDb, -55.f, -42.f));
        const float boost = airBoostS.process(params.airBoostDb) * activity * presence;
        const float lift = std::min(3.f, ak.airLiftFrac * R);

        const size_t r = (size_t) (g & (uint64_t) (kRing - 1));
        ringR[r] = R;
        ringUp[r] = lift + boost;
        // Sibilance gate for the output stage: opens INSTANTLY (an onset must never wait for a smoother) and closes
        // with a ~6 ms tail. It looks 2 frames ahead so it is already open when a short "s" arrives.
        const FrameFeatures& f1 = feats[(size_t) ((sg + 1) % kHist)];
        const float gTarget = clampf(3.f * std::max(fg.s, f1.s), 0.f, 1.f);
        ringGate[r] = std::max(gTarget, gateS.process(gTarget));
        ringFc[r] = fc;
    }

    // FINE path: exact spectral mask (WHERE) -> sibilant component S(t) by overlap-add.
    void processLongFrame(uint64_t F)
    {
        const uint64_t start = F * (uint64_t) hL;
        std::fill(PL.begin(), PL.end(), 0.f);
        for (int c = 0; c < nCh; ++c)
        {
            auto& ch = chan[(size_t) c];
            for (int i = 0; i < nL; ++i)
            {
                workRe[(size_t) i] = ch.inRing[(start + (uint64_t) i) & inMask] * windowL[(size_t) i];
                workIm[(size_t) i] = 0.f;
            }
            fftL.forward(workRe.data(), workIm.data());
            std::copy(workRe.begin(), workRe.begin() + nBinsL, ch.re.begin());
            std::copy(workIm.begin(), workIm.begin() + nBinsL, ch.im.begin());
            for (int k = 0; k < nBinsL; ++k)
                PL[(size_t) k] += workRe[(size_t) k] * workRe[(size_t) k] + workIm[(size_t) k] * workIm[(size_t) k];
        }
        for (int k = 0; k < nBinsL; ++k) PL[(size_t) k] *= normPL;

        // frequency tracked at the centre of this long frame
        const uint64_t centre = start + (uint64_t) (nL / 2) - (uint64_t) (nS / 2);
        const float fc = ringFc[(size_t) ((centre / (uint64_t) hS) & (uint64_t) (kRing - 1))];

        computeSpecMask(fc);
        const float* spec = params.precision > 0.001f ? specMask.data() : nullptr;
        if (params.listen == 2) zone.buildDetector(maskCurve.data(), fc, ak, spec, params.precision);
        else                    zone.buildMask(maskCurve.data(), fc, ak, spec, params.precision);

        // Calibrated: analysis and synthesis windows are sqrt-Hann, so their product is Hann; Hann at hop N/4 sums
        // to 2 (COLA), the inverse FFT is unnormalised (x N), hence 1/(2N)... = 0.5/N. Verified numerically: with mask = 1
        // everywhere and R = 80 dB the output is -79 dB below the input (S == dry), i.e. S is not under-scaled.
        const float scale = 0.5f / (float) nL;
        for (int c = 0; c < nCh; ++c)
        {
            auto& ch = chan[(size_t) c];
            for (int k = 0; k < nBinsL; ++k)
            {
                workRe[(size_t) k] = ch.re[(size_t) k] * maskCurve[(size_t) k];
                workIm[(size_t) k] = ch.im[(size_t) k] * maskCurve[(size_t) k];
            }
            workIm[0] = 0.f;
            workIm[(size_t) (nL / 2)] = 0.f;
            for (int k = 1; k < nL / 2; ++k)
            {
                workRe[(size_t) (nL - k)] = workRe[(size_t) k];
                workIm[(size_t) (nL - k)] = -workIm[(size_t) k];
            }
            fftL.inverse(workRe.data(), workIm.data());
            for (int i = 0; i < nL; ++i)
                ch.sRing[(start + (uint64_t) i) & outMask] += workRe[(size_t) i] * windowL[(size_t) i] * scale;
        }

        // meters for the UI: spectrum and the gain curve at the current reduction
        const float oneMinusG = 1.f - std::exp(-reductionDb.load(std::memory_order_relaxed) * 0.11512925f);
        for (int p = 0; p < kMeterPoints; ++p)
        {
            const size_t b = (size_t) meterBin[(size_t) p];
            specMeter[(size_t) p].store(powToDb(PL[b]), std::memory_order_relaxed);
            gainMeter[(size_t) p].store(params.listen == 2 ? 0.f : 20.f * std::log10(std::max(1.f - oneMinusG * maskCurve[b], 1.0e-3f)),
                                        std::memory_order_relaxed);
        }
    }

    // Per-bin mask of WHERE this sibilant has energy, on the fine (47 Hz) spectrum: bins within D dB of
    // the strongest sibilance peak (two humps, e.g. "sh" + "s", are both caught; valleys are left alone).
    void computeSpecMask(float fc)
    {
        const int kA = kSpecLo, kB = kSpecHi;
        const float lfcNow = std::log2(std::max(fc, 1000.f)), zoneSigma = ak.sigmaOct;
        float peak = -1.0e9f;
        maskPre[0] = 0.f;
        for (int k = 0; k < nBinsL; ++k) maskPre[(size_t) k + 1] = maskPre[(size_t) k] + PL[(size_t) k];
        for (int k = kA; k <= kB; ++k)
        {
            // power averaged over ~0.05 oct (not 3 bins): the random dips of a noisy "s" no longer punch holes in the
            // mask (a hole = mask<1 = a ceiling on how deep that bin can be cut), while the humps stay resolved
            const int h = std::max(1, (int) std::lround(0.035f * (float) k));
            const int lo = std::max(0, k - h), hi = std::min(nBinsL - 1, k + h);
            psDb[(size_t) k] = powToDb((maskPre[(size_t) hi + 1] - maskPre[(size_t) lo]) / (float) (hi - lo + 1));
            peak = std::max(peak, psDb[(size_t) k]);
        }
        // Width also limits how far below the peak a bin may be and still count as sibilance (narrow Width = only the peaks)
        // The deeper the cut being asked for, the more of the sibilant's flanks must be inside the mask: a deep cut
        // that only touches the peak leaves the sides of the "s" audible (extra tolerance up to +kDepthWiden dB).
        const float depthWiden = kDepthWiden * clampf(reductionDb.load(std::memory_order_relaxed) / 20.f, 0.f, 1.f);
        const float D = lerpf(18.f, 6.f, params.precision) * lerpf(0.6f, 1.f, widthN) + depthWiden;
        std::fill(maskRaw.begin(), maskRaw.end(), 0.f);
        for (int k = kA; k <= kB; ++k)
            maskRaw[(size_t) k] = ramp(psDb[(size_t) k] - peak, -D - 3.f, -D + 3.f);

        // light blur in log-frequency; Width controls how much (0.03 .. 0.12 octaves)
        const float sigmaB = 0.02f + 0.06f * widthN;   // edge blur of the peaks: 0.02..0.08 oct
        maskPre[0] = 0.f;
        for (int k = 0; k < nBinsL; ++k) maskPre[(size_t) k + 1] = maskPre[(size_t) k] + maskRaw[(size_t) k];
        for (int k = 0; k < nBinsL; ++k)
        {
            float target = 0.f;
            if (k >= kA && k <= kB)
            {
                const int h = (int) std::lround(sigmaB * 0.693f * (float) k);
                const int lo = std::max(kA, k - h), hi = std::min(kB, k + h);
                target = (maskPre[(size_t) hi + 1] - maskPre[(size_t) lo]) / (float) (hi - lo + 1);
            }
            // support window: the spectral mask can never reach far from the tracked frequency (2.6 sigma),
            // so a narrow Width really removes the bins away from the "s" instead of only blurring them
            if (target > 0.f)
            {
                const float d = (std::log2(std::max(1.f, (float) k * sr / (float) nL)) - lfcNow) / (2.6f * zoneSigma);
                target *= std::exp(-0.5f * d * d * d * d);
            }
            // Fast "s" fix: the spectrum of a 10-30 ms "s" is noisy, so the target mask jumps from frame to frame and the
            // flanks of the sibilant kept getting dropped. Rise INSTANTLY to any bin that shows sibilance (union over the event) and
            // let go slowly, so the whole body of the "s" stays covered for its entire (short) life.
            specMask[(size_t) k] += (target > specMask[(size_t) k] ? 1.f : aMaskFall) * (target - specMask[(size_t) k]);
        }
    }

    // configuration
    float sr = 48000.f, frameRate = 750.f, normPS = 1.f, normPL = 1.f, aFc = 0.1f, aMask = 0.7f, aMaskFall = 0.25f;
    static constexpr float kMaskHoldMs = 20.f, kDepthWiden = 4.f;
    int nCh = 2, nS = 256, hS = 64, nBinsS = 129, nL = 1024, hL = 256, nBinsL = 513, L = 1024;
    int kSpecLo = 8, kSpecHi = 300;
    bool prepared = false;
    Params params;
    AirKeepParams ak { 1.f, 0.f, 0.f, 0.4f, 0.7f, 3.f };
    float guardMaxR = 100.f, widthN = 0.3f, holdVal = 0.f;
    int holdCnt = 0;
    float lastAttack = -1.f, lastRelease = -1.f;
    float inGainT = 1.f, outGainT = 1.f, mixT = 1.f, bypassT = 0.f, deltaT = 0.f;
    OnePole inGainS, outGainS, mixS, bypassS, deltaS, airBoostS, activityS, presenceS, gateS;
    float lfcS = 12.77f;

    // state
    uint64_t nIn = 0, inMask = 0, outMask = 0;
    Fft fftS, fftL;
    std::vector<float> windowS, windowL, workRe, workIm, PS, PL, maskCurve;
    std::vector<float> psDb, maskRaw, maskPre, specMask;
    std::array<Chan, kMaxChannels> chan;
    std::array<FrameFeatures, kHist> feats;
    std::array<float, kRing> ringR {}, ringUp {}, ringGate {}, ringFc {};
    FeatureExtractor extractor;
    AdaptiveThreshold adapt;
    FrequencyTracker tracker;
    EnvelopeSmoother env;
    ZoneMask zone;

    // meters
    std::array<int, kMeterPoints> meterBin {};
    std::array<std::atomic<float>, kMeterPoints> gainMeter {}, specMeter {};
    std::atomic<float> dbgSfm { 0.f }, dbgRel { 0.f }, dbgRatio { 0.f }, dbgScore { 0.f }, dbgThr { 0.f }, dbgRaw { 0.f };
    std::atomic<float> reductionDb { 0.f }, fcMeter { 7000.f }, peakInM { 0.f }, peakOutM { 0.f };
};

} // namespace naturalair
