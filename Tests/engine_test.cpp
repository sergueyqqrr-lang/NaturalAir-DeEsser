// Standalone tests for the DSP engine (no JUCE needed).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "../Source/DSP/DeEsserEngine.h"

using namespace naturalair;


#ifndef TEST_SEED
#define TEST_SEED 1234
#endif
// Deterministic noise: identical on every compiler/platform (std::normal_distribution is not).
struct Gauss
{
    float sigma; uint64_t st;
    Gauss(float s, uint64_t seed) : sigma(s), st(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    float u()
    {
        st ^= st >> 12; st ^= st << 25; st ^= st >> 27;
        return (float) (((st * 0x2545F4914F6CDD1Dull) >> 40) + 1) / 16777217.f;   // (0,1)
    }
    float operator()() { return sigma * std::sqrt(-2.f * std::log(u())) * std::cos(6.2831853f * u()); }
};

static int failures = 0;
#define CHECK(cond, ...)                                                          \
    do { if (!(cond)) { ++failures; std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void runBlocks(DeEsserEngine& e, std::vector<std::vector<float>>& ch, int blockSize)
{
    const int total = (int) ch[0].size();
    std::vector<float*> ptrs(ch.size());
    for (int pos = 0; pos < total; pos += blockSize)
    {
        const int n = std::min(blockSize, total - pos);
        for (size_t c = 0; c < ch.size(); ++c) ptrs[c] = ch[c].data() + pos;
        e.process(ptrs.data(), n);
    }
}


// RBJ 2nd-order high-pass, applied twice, so the synthetic sibilance has the same
// spectrum at every sample rate.
struct HP2
{
    double b0, b1, b2, a1, a2, z1[2] {}, z2[2] {};
    HP2(double fs, double fc)
    {
        const double w = 2 * kPi * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * 0.7071);
        const double a0 = 1 + al;
        b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
    }
    float process(float x)
    {
        double v = x;
        for (int s = 0; s < 2; ++s)
        {
            const double y = b0 * v + z1[s];
            z1[s] = b1 * v - a1 * y + z2[s];
            z2[s] = b2 * v - a2 * y;
            v = y;
        }
        return (float) v;
    }
};

// ---------- synthetic "voice" ----------
struct Synth
{
    std::vector<float> x;
    std::vector<std::pair<double, double>> sEvents; // seconds
    std::vector<std::pair<double, double>> breaths;
};

static Synth makeVoice(double sr, double seconds)
{
    Synth o;
    const int n = (int) (sr * seconds);
    o.x.assign((size_t) n, 0.f);
    Gauss g(1.f, TEST_SEED);
    // vowel: harmonic series with formant-ish roll-off and slow vibrato/amplitude change
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const double f0 = 140.0 * (1.0 + 0.01 * std::sin(2 * kPi * 5.0 * t));
        double v = 0;
        for (int h = 1; h <= 60 && h * f0 < 0.45 * sr; ++h)
            v += std::sin(2 * kPi * h * f0 * t) * std::exp(-h * f0 / 2200.0) / h * 3.0;
        o.x[(size_t) i] = (float) (0.15 * v * (0.8 + 0.2 * std::sin(2 * kPi * 1.3 * t)));
    }
    // sibilance events: bright noise (differentiated white noise), vowel dips under it
    const double ev[][2] = { { 0.8, 0.95 }, { 1.8, 1.92 }, { 2.8, 3.0 }, { 3.8, 3.9 } };
    const float amp[] = { 0.10f, 0.14f, 0.07f, 0.12f };
    int e = 0;
    for (auto& w : ev)
    {
        o.sEvents.push_back({ w[0], w[1] });
        const int a = (int) (w[0] * sr), b = std::min(n, (int) (w[1] * sr));
        if (a >= n) { ++e; continue; }
        HP2 hpf(sr, 4500.0);
        const float psdScale = (float) std::sqrt(sr / 48000.0);
        for (int i = a; i < b; ++i)
        {
            const float nz = g();
            const float hp = hpf.process(nz);
            const double ph = (double) (i - a) / (b - a);
            const float env = (float) std::min({ 1.0, ph / 0.125, (1.0 - ph) / 0.1 });
            o.x[(size_t) i] = o.x[(size_t) i] * 0.15f + 0.45f * amp[e] * psdScale * hp * env;
        }
        ++e;
    }
    // breath (flat noise, low level)
    o.breaths.push_back({ 4.3, 4.6 });
    for (int i = (int) (4.3 * sr); i < std::min(n, (int) (4.6 * sr)); ++i) o.x[(size_t) i] = 0.004f * g();
    return o;
}

static bool testNull(double sr, int block)
{
    auto ep = std::make_unique<DeEsserEngine>();
    DeEsserEngine& e = *ep;
    e.prepare(sr, 2);
    Params p; p.rangeDb = 0.f; p.airBoostDb = 0.f; p.mix = 1.f;
    e.setParams(p); e.reset();
    const int n = (int) sr * 2;
    std::vector<std::vector<float>> ch(2, std::vector<float>((size_t) n));
    Gauss g(0.1f, 7);
    for (int i = 0; i < n; ++i) { ch[0][(size_t) i] = g() + 0.3f * std::sin(0.05f * i); ch[1][(size_t) i] = g() * 0.5f; }
    auto ref = ch;
    runBlocks(e, ch, block);
    const int L = e.getLatencySamples();
    double errE = 0, refE = 0;
    for (int c = 0; c < 2; ++c)
        for (int i = L + 4096; i < n; ++i)
        {
            const double d = (double) ch[(size_t) c][(size_t) i] - (double) ref[(size_t) c][(size_t) (i - L)];
            errE += d * d; refE += (double) ref[(size_t) c][(size_t) (i - L)] * ref[(size_t) c][(size_t) (i - L)];
        }
    const double db = 10 * std::log10(errE / refE + 1e-30);
    std::printf("  null test sr=%.0f block=%4d latency=%d samples (%.1f ms): error %.1f dB\n", sr, block, L, 1000.0 * L / sr, db);
    CHECK(db < -90.0, "null test error %.1f dB (expected < -90)", db);
    return db < -90.0;
}

static void testSynthetic(double sr)
{
    std::printf("synthetic voice test @ %.0f Hz\n", sr);
    Synth v = makeVoice(sr, 5.0);
    for (int adaptive = 0; adaptive < 2; ++adaptive)
    {
        auto ep = std::make_unique<DeEsserEngine>();
        DeEsserEngine& e = *ep;
        e.prepare(sr, 1);
        Params p; p.rangeDb = 24.f; p.airKeep = 0.65f; p.adaptive = adaptive != 0; p.threshold = 0.5f;
        e.setParams(p); e.reset();
        const int n = (int) v.x.size();
        std::vector<std::vector<float>> ch(1, v.x);
        std::vector<float> redTrace((size_t) n, 0.f);
        // process in 64-sample blocks to also trace the meter
        float* ptr[1];
        for (int pos = 0; pos < n; pos += 64)
        {
            const int m = std::min(64, n - pos);
            ptr[0] = ch[0].data() + pos;
            e.process(ptr, m);
            for (int i = 0; i < m; ++i) redTrace[(size_t) (pos + i)] = e.getReductionDb();
        }
        const int L = e.getLatencySamples();
        std::vector<float> evMax;
        // maximum reduction near each sibilant event (meter is already latency-aligned to output)
        for (auto& w : v.sEvents)
        {
            const int a = (int) (w.first * sr), b = (int) (w.second * sr) + L;
            float mx = 0; for (int i = a; i < b; ++i) mx = std::max(mx, redTrace[(size_t) i]);
            std::printf("  adaptive=%d  event %.2f-%.2fs  max reduction %.1f dB\n", adaptive, w.first, w.second, mx);
            evMax.push_back(mx);
            CHECK(mx > 3.0f, "sibilance at %.2fs not reduced (%.1f dB)", w.first, mx);
        }
        // false positives: vowel-only and breath segments
        float fp = 0;
        int fpAt = 0;
        for (int i = (int) (0.15 * sr); i < (int) (0.7 * sr); ++i) if (redTrace[(size_t) i] > fp) { fp = redTrace[(size_t) i]; fpAt = i; }
        for (int i = (int) (4.45 * sr); i < (int) (4.65 * sr); ++i) if (redTrace[(size_t) i] > fp) { fp = redTrace[(size_t) i]; fpAt = i; }
        std::printf("  adaptive=%d  max reduction on vowel/breath-only: %.2f dB (at %.3fs)\n", adaptive, fp, fpAt / sr);
        CHECK(fp < 0.5f, "false positives on non-sibilant material: %.2f dB", fp);
        // delta on vowel-only section must be ~0
        double errE = 0, refE = 0;
        for (int i = (int) (0.3 * sr); i < (int) (0.7 * sr); ++i)
        {
            const double d = (double) ch[0][(size_t) (i + L)] - (double) v.x[(size_t) i];
            errE += d * d; refE += (double) v.x[(size_t) i] * v.x[(size_t) i];
        }
        const double db = 10 * std::log10(errE / refE + 1e-30);
        std::printf("  adaptive=%d  vowel-only delta: %.1f dB\n", adaptive, db);
        CHECK(db < -60.0, "vowel section altered: %.1f dB", db);
        for (float s : ch[0]) CHECK(std::isfinite(s), "non-finite output");
    }
}

static void testModes(double sr)
{
    std::printf("modes test @ %.0f Hz\n", sr);
    // Listen modes / bypass / boost must stay finite and bounded.
    Synth v = makeVoice(sr, 5.0);
    for (int mode = 0; mode < 7; ++mode)
    {
        auto ep = std::make_unique<DeEsserEngine>();
        DeEsserEngine& e = *ep; e.prepare(sr, 2);
        Params p; p.airBoostDb = 3.f; p.airKeep = 1.f;
        p.listen = mode == 1 ? 1 : (mode == 2 ? 2 : 0);
        p.bypass = mode == 3; p.mix = mode == 4 ? 0.5f : 1.f;
        p.freqAuto = mode != 4; p.freqHz = 9000.f;
        if (mode == 5) { p.precision = 1.f; p.attackMs = 0.01f; p.releaseMs = 0.5f; }     // extreme fast
        if (mode == 6) { p.precision = 0.f; p.attackMs = 200.f;  p.releaseMs = 3000.f; }  // extreme slow
        e.setParams(p); e.reset();
        std::vector<std::vector<float>> ch(2, v.x);
        runBlocks(e, ch, 100);
        float mx = 0; for (auto& c : ch) for (float s : c) { CHECK(std::isfinite(s), "NaN in mode %d", mode); mx = std::max(mx, std::fabs(s)); }
        CHECK(mx < 4.f, "output too large in mode %d: %f", mode, mx);
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash must not swallow the progress log
    std::printf("== null / transparency ==\n");
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int block : { 1, 37, 512 })
            if (!(sr > 100000 && block == 1)) testNull(sr, block);
    std::printf("== synthetic voice ==\n");
    testSynthetic(48000.0);
    testSynthetic(44100.0);
    testSynthetic(96000.0);
    std::printf("== listen/bypass/boost modes ==\n");
    testModes(48000.0);
    if (failures) { std::printf("\n%d FAILURE(S)\n", failures); return 1; }
    std::printf("\nALL TESTS PASSED\n");
    return 0;
}
