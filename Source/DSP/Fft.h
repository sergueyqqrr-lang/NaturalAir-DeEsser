#pragma once
#include <cmath>
#include <utility>
#include <vector>

namespace naturalair {

constexpr double kPi = 3.14159265358979323846;

// Radix-2 complex FFT (split re/im arrays). Inverse is unnormalised.
class Fft
{
public:
    void init(int n)
    {
        N = n;
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        rev.assign((size_t) n, 0);
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[(size_t) i] = r;
        }
        cosT.resize((size_t) n / 2);
        sinT.resize((size_t) n / 2);
        for (int i = 0; i < n / 2; ++i)
        {
            cosT[(size_t) i] = (float) std::cos(2.0 * kPi * i / n);
            sinT[(size_t) i] = (float) std::sin(2.0 * kPi * i / n);
        }
    }

    void forward(float* re, float* im) const { transform(re, im, false); }
    void inverse(float* re, float* im) const { transform(re, im, true); }
    int size() const { return N; }

private:
    void transform(float* re, float* im, bool inv) const
    {
        for (int i = 0; i < N; ++i)
        {
            const int j = rev[(size_t) i];
            if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        for (int len = 2; len <= N; len <<= 1)
        {
            const int half = len >> 1, step = N / len;
            for (int i = 0; i < N; i += len)
                for (int k = 0; k < half; ++k)
                {
                    const float wr = cosT[(size_t) (k * step)];
                    const float wi = inv ? sinT[(size_t) (k * step)] : -sinT[(size_t) (k * step)];
                    const int a = i + k, b = a + half;
                    const float tr = re[b] * wr - im[b] * wi;
                    const float ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr;        im[a] += ti;
                }
        }
    }

    int N = 0;
    std::vector<int> rev;
    std::vector<float> cosT, sinT;
};

} // namespace naturalair
