// Auto-frequency ZONE accuracy test (no JUCE needed).
//
// One "take": a vowel with sibilants of very different colour in the same file (5-6 kHz, 7-9 kHz, a "sh"+"s" two-hump
// one, short and long). For every sibilant it measures
//   - the Frequency meter at the middle of the event vs the true centre,
//   - where the reduction mask really sits (mask centroid / coverage of the sibilant's own energy / spill outside it),
//   - that nothing below 4 kHz is ever touched.
// Run:  g++ -O2 -std=c++17 Tests/zone_test.cpp -o zone_test && ./zone_test [precision=0.75] [widthOct=0.25] [sampleRate=48000]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "../Source/DSP/DeEsserEngine.h"

using namespace naturalair;

struct Gauss
{
    float sigma; uint64_t st;
    Gauss(float s, uint64_t seed) : sigma(s), st(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    float u()
    {
        st ^= st >> 12; st ^= st << 25; st ^= st >> 27;
        return (float) (((st * 0x2545F4914F6CDD1Dull) >> 40) + 1) / 16777217.f;
    }
    float operator()() { return sigma * std::sqrt(-2.f * std::log(u())) * std::cos(6.2831853f * u()); }
};

struct BP
{
    double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    BP(double fs, double fc, double q)
    {
        const double w = 2 * kPi * fc / fs, al = std::sin(w) / (2 * q), a0 = 1 + al;
        b0 = al / a0; b1 = 0; b2 = -al / a0; a1 = -2 * std::cos(w) / a0; a2 = (1 - al) / a0;
    }
    float process(float x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return (float) y; }
};

// TPT state-variable band-pass (unity gain at the centre), centre frequency can change every sample
struct SVF
{
    double ic1 = 0, ic2 = 0;
    float process(float x, double fs, double fc, double q)
    {
        const double g = std::tan(kPi * fc / fs), k = 1.0 / q, a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const double v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
        return (float) (k * v1);
    }
};

// kind 0: one hump at f1.   kind 1: two humps f1 + f2 (equal level).   kind 2: one hump gliding f1 -> f2 during the event.
struct Ev { double f1, f2; double q; double durMs; const char* name; int kind; double lvlDb; };

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const double sr = argc > 3 ? std::atof(argv[3]) : 48000.0;
    const float precision = argc > 1 ? (float) std::atof(argv[1]) : 0.75f;
    const float widthOct = argc > 2 ? (float) std::atof(argv[2]) : 0.25f;
    // alternating colours on purpose: a tracker that drags to the history/previous "s" is exposed
    const std::vector<Ev> evs = {
        { 7000, 0,    1.5, 120, "s 7.0k",            0, -14 }, { 5400, 0,    1.5, 120, "s 5.4k",            0, -14 },
        { 8800, 0,    1.5, 120, "s 8.8k",            0, -14 }, { 5800, 0,    1.5, 70,  "s 5.8k short",      0, -14 },
        { 8200, 0,    1.5, 60,  "s 8.2k short",      0, -14 }, { 6000, 0,    1.5, 100, "weak s 6.0k",       0, -23 },
        { 8500, 0,    1.5, 100, "weak s 8.5k",       0, -23 }, { 5500, 7000, 1.5, 150, "glide 5.5k->7.0k",  2, -14 },
        { 9000, 7000, 1.5, 150, "glide 9.0k->7.0k",  2, -14 }, { 5200, 8500, 2.5, 140, "sh 5.2k + s 8.5k",  1, -14 },
        { 7500, 0,    1.5, 120, "s 7.5k",            0, -14 }, { 5300, 8800, 2.5, 140, "sh 5.3k + s 8.8k",  1, -14 },
        { 4800, 0,    0.9, 140, "broad sh 4.8k",     0, -14 }, { 9200, 0,    1.5, 110, "s 9.2k",            0, -14 },
    };
    const double lead = 0.5, gap = 0.5;
    const int n = (int) (sr * (lead + gap * (double) evs.size() + 0.4));
    std::vector<float> x((size_t) n), vow((size_t) n), sib((size_t) n, 0.f);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr; double v = 0;
        for (int h = 1; h <= 60 && h * 150.0 < 0.45 * sr; ++h) v += std::sin(2 * kPi * h * 150.0 * t) * std::exp(-h * 150.0 / 2200.0) / h * 3.0;
        vow[(size_t) i] = (float) (0.15 * v);
    }
    double vr = 0; for (float v : vow) vr += (double) v * v; vr = std::sqrt(vr / n);

    std::vector<int> st, ln;
    Gauss g(1.f, 4242);
    for (size_t e = 0; e < evs.size(); ++e)
    {
        const Ev& ev = evs[e];
        const int a = (int) ((lead + gap * (double) e) * sr), len = (int) (ev.durMs * 0.001 * sr);
        st.push_back(a); ln.push_back(len);
        SVF fa1, fa2, fb1, fb2;
        std::vector<float> nz((size_t) len); double nr = 0;
        for (int i = 0; i < len; ++i)
        {
            const double t = (double) i / len;
            float v;
            if (ev.kind == 2)
            {
                const double fcI = ev.f1 * std::pow(ev.f2 / ev.f1, t);
                v = fa2.process(fa1.process(g(), sr, fcI, ev.q), sr, fcI, ev.q);
            }
            else
            {
                v = fa2.process(fa1.process(g(), sr, ev.f1, ev.q), sr, ev.f1, ev.q);
                if (ev.kind == 1) v += fb2.process(fb1.process(g(), sr, ev.f2, ev.q), sr, ev.f2, ev.q);
            }
            nz[(size_t) i] = v; nr += (double) v * v;
        }
        const float gain = (float) (vr * std::pow(10.0, ev.lvlDb / 20.0) / std::sqrt(nr / len));
        for (int i = 0; i < len; ++i)
        {
            const double ph = (double) i / len;
            sib[(size_t) (a + i)] = gain * nz[(size_t) i] * (float) std::min({ 1.0, ph / 0.10, (1.0 - ph) / 0.15 });
        }
        for (int i = -(int) (0.01 * sr); i < len + (int) (0.01 * sr); ++i) if (a + i >= 0 && a + i < n) vow[(size_t) (a + i)] *= 0.15f;
    }
    for (int i = 0; i < n; ++i) x[(size_t) i] = vow[(size_t) i] + sib[(size_t) i];

    auto ep = std::make_unique<DeEsserEngine>(); DeEsserEngine& e = *ep;
    e.prepare(sr, 1);
    Params p; p.rangeDb = 16.f; p.widthOct = widthOct; p.precision = precision; e.setParams(p); e.reset();
    const int L = e.getLatencySamples();
    const int nL = L, binsL = nL / 2 + 1;
    const float binHz = (float) (sr / nL);
    std::vector<float> out(x), fcTr((size_t) n, 0.f);
    std::vector<std::vector<float>> maskSnap(evs.size());
    size_t nextEv = 0;
    for (int pos = 0; pos < n; pos += 32)
    {
        const int m = std::min(32, n - pos); float* pp = out.data() + pos; e.process(&pp, m);
        for (int i = 0; i < m; ++i) fcTr[(size_t) (pos + i)] = e.getFrequencyHz();
        // mask for the middle of event k is built when the input head reaches mid + nL/2
        if (nextEv < evs.size() && pos + m >= st[nextEv] + ln[nextEv] / 2 + nL / 2) { maskSnap[nextEv] = e.debugMask(); ++nextEv; }
    }

    Fft fft; fft.init(nL);
    std::vector<float> win((size_t) nL);
    for (int i = 0; i < nL; ++i) win[(size_t) i] = 0.5f - 0.5f * (float) std::cos(2.0 * kPi * i / nL);

    std::printf("precision=%.2f width=%.2f oct\n", precision, widthOct);
    std::printf("%-20s %7s | %7s %6s | %8s %8s | %8s %7s %7s\n", "event", "true", "meter", "err%", "maskCen", "cenErr%", "coverage", "spill", "<4k");
    double sumErr = 0; int nErr = 0; double sumCov = 0, sumSpill = 0;
    for (size_t k = 0; k < evs.size(); ++k)
    {
        const Ev& ev = evs[k];
        const int mid = st[k] + ln[k] / 2;
        // Frequency meter, read where the control frame for each instant is on show (25 / 50 / 75 % of the event)
        double errM = 0, fm = 0;
        for (double ph : { 0.25, 0.5, 0.75 })
        {
            const double fmI = fcTr[(size_t) std::min(n - 1, (int) (st[k] + ph * ln[k]) + L / 2)];
            double e1;
            if (ev.kind == 1) e1 = std::min(std::fabs(fmI / ev.f1 - 1), std::fabs(fmI / ev.f2 - 1));
            else { const double tru = ev.kind == 2 ? ev.f1 * std::pow(ev.f2 / ev.f1, ph) : ev.f1; e1 = std::fabs(fmI / tru - 1); }
            errM += e1 * 100 / 3;
            if (ph == 0.5) fm = fmI;
        }
        // true spectrum of the sibilant itself (the fine analysis window around the middle)
        std::vector<float> re((size_t) nL), im((size_t) nL, 0.f);
        for (int i = 0; i < nL; ++i) { const int j = mid - nL / 2 + i; re[(size_t) i] = (j >= 0 && j < n ? sib[(size_t) j] : 0.f) * win[(size_t) i]; }
        fft.forward(re.data(), im.data());
        std::vector<double> P((size_t) binsL);
        double pk = 0; for (int b = 0; b < binsL; ++b) { P[(size_t) b] = (double) re[(size_t) b] * re[(size_t) b] + (double) im[(size_t) b] * im[(size_t) b]; if (b * binHz > 4000 && b * binHz < 16000) pk = std::max(pk, P[(size_t) b]); }
        const auto& M = maskSnap[k];
        double mc = 0, mw = 0, cov = 0, tot = 0, spill = 0, mtot = 0, low = 0;
        for (int b = 1; b < binsL; ++b)
        {
            const double f = b * binHz; const double mk = M.empty() ? 0.0 : M[(size_t) b];
            if (f < 3500) { low = std::max(low, mk); continue; }
            if (f < 4000) continue;
            if (f > 16000) continue;
            tot += P[(size_t) b]; cov += P[(size_t) b] * std::min(1.0, mk);
            mc += std::log2(f) * mk * P[(size_t) b]; mw += mk * P[(size_t) b];
            mtot += mk;
            if (P[(size_t) b] < pk * 0.05) spill += mk;   // mask on bins where the sibilant has < -13 dB of its peak
        }
        const double mcHz = mw > 0 ? std::exp2(mc / mw) : 0.0;
        const double ref = ev.kind != 0 ? std::sqrt(ev.f1 * ev.f2) : ev.f1;
        const double errC = (mcHz / ref - 1) * 100;
        const double coverage = tot > 0 ? cov / tot : 0, spillF = mtot > 0 ? spill / mtot : 0;
        std::printf("%-20s %7.0f | %7.0f %5.1f%% | %8.0f %+7.1f%% | %7.0f%% %6.0f%% %7.2f\n", ev.name, ref, fm, errM, mcHz, errC, coverage * 100, spillF * 100, low);
        if (ev.kind != 1) { sumErr += errM; ++nErr; }
        sumCov += coverage; sumSpill += spillF;
        CHECK(low < 0.05, "%s: mask below 3.5 kHz = %.3f", ev.name, low);
        for (int i = 0; i < ln[k]; ++i) CHECK(std::isfinite(out[(size_t) (st[k] + i)]), "non-finite");
    }
    std::printf("mean |meter error| over 25-50-75%% of single-hump events: %.1f %%   mean coverage %.0f %%   mean spill %.0f %%\n", sumErr / nErr, 100 * sumCov / evs.size(), 100 * sumSpill / evs.size());
    std::printf("%s\n", failures ? "ZONE TEST: FAIL" : "ZONE TEST: ok");
    return failures ? 1 : 0;
}
