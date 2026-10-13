#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace naturalair {

constexpr float kEps = 1.0e-20f;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float smoothstep(float e0, float e1, float x)
{
    const float t = clampf((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
inline float ramp(float x, float lo, float hi) { return smoothstep(lo, hi, x); }
inline float dbToLin(float db) { return std::pow(10.f, db * 0.05f); }
inline float powToDb(float p) { return 10.f * std::log10(std::max(p, kEps)); }
inline int nextPow2(int x) { int p = 1; while (p < x) p <<= 1; return p; }

// One-pole smoother with time constant in seconds at sample/frame rate fs.
struct OnePole
{
    float a = 1.f, y = 0.f;
    void setTime(float seconds, float fs) { a = seconds <= 0.f ? 1.f : 1.f - std::exp(-1.f / (seconds * fs)); }
    void snap(float v) { y = v; }
    float process(float x) { y += (x - y) * a; return y; }
};

} // namespace naturalair
