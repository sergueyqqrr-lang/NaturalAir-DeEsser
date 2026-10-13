// Offline tool: runs a 16-bit PCM WAV (mono/stereo) through the engine.
//   process_wav in.wav out.wav [delta.wav] [trace.csv] [key=value ...]
// keys: airKeep threshold range attack(ms) release(ms) adaptive freqAuto freq airBoost  (airKeep/threshold in 0..100)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include "../Source/DSP/DeEsserEngine.h"

struct Wav { int sr = 48000, ch = 1; std::vector<std::vector<float>> d; };

static bool readWav(const char* path, Wav& w)
{
    std::ifstream f(path, std::ios::binary); if (!f) return false;
    std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, &b[o], 4); return v; };
    auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, &b[o], 2); return v; };
    size_t pos = 12; int bits = 16;
    while (pos + 8 <= b.size())
    {
        const uint32_t sz = u32(pos + 4);
        if (!std::memcmp(&b[pos], "fmt ", 4)) { w.ch = u16(pos + 10); w.sr = (int) u32(pos + 12); bits = u16(pos + 22); }
        if (!std::memcmp(&b[pos], "data", 4))
        {
            if (bits != 16) return false;
            const size_t n = sz / 2 / (size_t) w.ch;
            w.d.assign((size_t) w.ch, std::vector<float>(n));
            for (size_t i = 0; i < n; ++i)
                for (int c = 0; c < w.ch; ++c) { int16_t v; std::memcpy(&v, &b[pos + 8 + (i * (size_t) w.ch + (size_t) c) * 2], 2); w.d[(size_t) c][i] = v / 32768.f; }
            return true;
        }
        pos += 8 + sz + (sz & 1);
    }
    return false;
}

static void writeWav(const char* path, const Wav& w)
{
    std::ofstream f(path, std::ios::binary);
    const uint32_t n = (uint32_t) w.d[0].size(), dataBytes = n * (uint32_t) w.ch * 2, rate = (uint32_t) w.sr;
    const uint16_t ch = (uint16_t) w.ch, bits = 16, align = (uint16_t) (ch * 2), fmt = 1;
    const uint32_t riff = 36 + dataBytes, fmtSz = 16, byteRate = rate * align;
    f.write("RIFF", 4); f.write((const char*) &riff, 4); f.write("WAVEfmt ", 8); f.write((const char*) &fmtSz, 4);
    f.write((const char*) &fmt, 2); f.write((const char*) &ch, 2); f.write((const char*) &rate, 4);
    f.write((const char*) &byteRate, 4); f.write((const char*) &align, 2); f.write((const char*) &bits, 2);
    f.write("data", 4); f.write((const char*) &dataBytes, 4);
    for (uint32_t i = 0; i < n; ++i)
        for (int c = 0; c < w.ch; ++c)
        {
            float x = std::max(-1.f, std::min(1.f, w.d[(size_t) c][i]));
            const int16_t v = (int16_t) std::lrintf(x * 32767.f); f.write((const char*) &v, 2);
        }
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: process_wav in.wav out.wav [delta.wav] [trace.csv] [key=value ...]\n"); return 1; }
    Wav in; if (!readWav(argv[1], in)) { std::printf("cannot read 16-bit PCM wav\n"); return 1; }
    naturalair::Params p;
    const char* deltaPath = nullptr; const char* csvPath = nullptr;
    for (int i = 3; i < argc; ++i)
    {
        std::string a = argv[i]; const auto eq = a.find('=');
        if (eq == std::string::npos) { (deltaPath ? csvPath : deltaPath) = argv[i]; continue; }
        const std::string k = a.substr(0, eq); const float v = (float) std::atof(a.c_str() + eq + 1);
        if (k == "airKeep") p.airKeep = v * 0.01f; else if (k == "threshold") p.threshold = v * 0.01f;
        else if (k == "range") p.rangeDb = v; else if (k == "width") p.widthOct = v; else if (k == "precision") p.precision = v * 0.01f; else if (k == "attack") p.attackMs = v; else if (k == "release") p.releaseMs = v;
        else if (k == "adaptive") p.adaptive = v > 0.5f; else if (k == "freqAuto") p.freqAuto = v > 0.5f;
        else if (k == "freq") p.freqHz = v; else if (k == "airBoost") p.airBoostDb = v; else if (k == "mix") p.mix = v * 0.01f;
    }
    auto e = std::make_unique<naturalair::DeEsserEngine>();
    e->prepare(in.sr, in.ch); e->setParams(p); e->reset();
    const int L = e->getLatencySamples();
    Wav out = in; const size_t n = in.d[0].size();
    for (auto& c : out.d) c.resize(n + (size_t) L, 0.f);
    for (int c = 0; c < in.ch; ++c) for (int i = 0; i < L; ++i) out.d[(size_t) c][n + (size_t) i] = 0.f;
    std::vector<float*> ptr((size_t) in.ch);
    std::FILE* csv = csvPath ? std::fopen(csvPath, "w") : nullptr;
    if (csv) std::fprintf(csv, "t,ratioDb,thresholdDb,score,reductionDb,fcHz,sfm,relDb\n");
    float maxR = 0; double sumR = 0; size_t blocks = 0, active = 0;
    for (size_t pos = 0; pos < n + (size_t) L; pos += 128)
    {
        const size_t m = std::min<size_t>(128, n + (size_t) L - pos);
        for (int c = 0; c < in.ch; ++c) ptr[(size_t) c] = out.d[(size_t) c].data() + pos;
        e->process(ptr.data(), (int) m);
        const float R = e->getReductionDb(); maxR = std::max(maxR, R); sumR += R; ++blocks; if (R > 0.5f) ++active;
        if (csv) std::fprintf(csv, "%.4f,%.2f,%.2f,%.3f,%.2f,%.0f,%.3f,%.1f\n", pos / (double) in.sr, e->getDebugRatioDb(), e->getDebugThresholdDb(), e->getDebugScore(), R, e->getFrequencyHz(), e->getDebugSfm(), e->getDebugRelDb());
    }
    if (csv) std::fclose(csv);
    // remove latency
    Wav res = in, dl = in;
    for (int c = 0; c < in.ch; ++c)
        for (size_t i = 0; i < n; ++i) { res.d[(size_t) c][i] = out.d[(size_t) c][i + (size_t) L]; dl.d[(size_t) c][i] = res.d[(size_t) c][i] - in.d[(size_t) c][i]; }
    writeWav(argv[2], res); if (deltaPath) writeWav(deltaPath, dl);
    std::printf("latency %d samples | max reduction %.1f dB | mean %.2f dB | time with >0.5 dB reduction: %.1f %%\n",
                L, maxR, sumR / std::max<size_t>(1, blocks), 100.0 * (double) active / (double) std::max<size_t>(1, blocks));
    return 0;
}
