// Small, dependency-free real FFT (split re/im output), radix-2 DIT on a half-size
// complex transform. Sizes are powers of two. Not thread-safe per instance (owns scratch).
// Drop-in replaceable by PFFFT / pocketfft behind the same interface if desired.
#pragma once
#include "aligned.hpp"
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cstdint>
#include <vector>
#include <utility>

namespace llc {

class ComplexFFT {
public:
    void init(size_t m) {
        m_ = m;
        pairs_.clear();
        const size_t bits = log2i(m);
        for (size_t i = 0; i < m; ++i) {
            size_t r = 0;
            for (size_t b = 0; b < bits; ++b) if (i & (size_t(1) << b)) r |= size_t(1) << (bits - 1 - b);
            if (i < r) pairs_.push_back({static_cast<uint32_t>(i), static_cast<uint32_t>(r)});
        }
        twr_.resize(m > 1 ? m : 1);
        twi_.resize(m > 1 ? m : 1);
        for (size_t half = 1; half < m; half <<= 1)
            for (size_t j = 0; j < half; ++j) {
                const double a = M_PI * double(j) / double(half);
                twr_[half - 1 + j] = float(std::cos(a));
                twi_[half - 1 + j] = float(-std::sin(a));
            }
    }

    // In-place forward transform, e^{-i...}, unnormalised.
    void forward(float* __restrict re, float* __restrict im) const {
        const size_t m = m_;
        for (const auto& p : pairs_) {
            std::swap(re[p.first], re[p.second]);
            std::swap(im[p.first], im[p.second]);
        }
        if (m >= 2)
            for (size_t i = 0; i < m; i += 2) {
                const float ar = re[i], ai = im[i], br = re[i + 1], bi = im[i + 1];
                re[i] = ar + br; im[i] = ai + bi; re[i + 1] = ar - br; im[i + 1] = ai - bi;
            }
        if (m >= 4)
            for (size_t i = 0; i < m; i += 4) {
                float ar = re[i], ai = im[i], br = re[i + 2], bi = im[i + 2];
                re[i] = ar + br; im[i] = ai + bi; re[i + 2] = ar - br; im[i + 2] = ai - bi;
                ar = re[i + 1]; ai = im[i + 1];
                br = im[i + 3]; bi = -re[i + 3];            // x[i+3] * (-i)
                re[i + 1] = ar + br; im[i + 1] = ai + bi; re[i + 3] = ar - br; im[i + 3] = ai - bi;
            }
        for (size_t half = 4; half < m; half <<= 1) {
            const float* wr = twr_.data() + (half - 1);
            const float* wi = twi_.data() + (half - 1);
            for (size_t base = 0; base < m; base += 2 * half) {
                float* __restrict ar = re + base;
                float* __restrict ai = im + base;
                float* __restrict br = re + base + half;
                float* __restrict bi = im + base + half;
                for (size_t j = 0; j < half; ++j) {
                    const float tr = br[j] * wr[j] - bi[j] * wi[j];
                    const float ti = br[j] * wi[j] + bi[j] * wr[j];
                    br[j] = ar[j] - tr; bi[j] = ai[j] - ti;
                    ar[j] += tr;        ai[j] += ti;
                }
            }
        }
    }
    // Unnormalised inverse (e^{+i...}); result is m times the original.
    void inverse(float* re, float* im) const { forward(im, re); }

private:
    static size_t log2i(size_t v) { size_t r = 0; while ((size_t(1) << r) < v) ++r; return r; }
    size_t m_ = 0;
    std::vector<std::pair<uint32_t, uint32_t>> pairs_;
    std::vector<float> twr_, twi_;
};

// Real FFT of size n (power of two, n >= 8). Spectrum has n/2+1 bins in split arrays.
// inverse() is unnormalised: inverse(forward(x)) == (n/2) * x.
class RealFFT {
public:
    void init(size_t n) {
        n_ = n; m_ = n / 2;
        cfft_.init(m_);
        zr_.resize(m_); zi_.resize(m_);
        wr_.resize(m_ + 1); wi_.resize(m_ + 1);
        for (size_t k = 0; k <= m_; ++k) {
            const double a = 2.0 * M_PI * double(k) / double(n);
            wr_[k] = float(std::cos(a));
            wi_[k] = float(-std::sin(a));
        }
    }
    size_t size() const { return n_; }
    size_t bins() const { return m_ + 1; }
    float inverseScale() const { return 1.0f / float(m_); }

    void forward(const float* in, float* re, float* im) {
        float* zr = zr_.data(); float* zi = zi_.data();
        for (size_t k = 0; k < m_; ++k) { zr[k] = in[2 * k]; zi[k] = in[2 * k + 1]; }
        cfft_.forward(zr, zi);
        for (size_t k = 0; k <= m_; ++k) {
            const size_t kk = (k == m_) ? 0 : k;
            const size_t mk = (k == 0 || k == m_) ? 0 : m_ - k;
            const float fer = 0.5f * (zr[kk] + zr[mk]);
            const float fei = 0.5f * (zi[kk] - zi[mk]);
            const float for_ = 0.5f * (zi[kk] + zi[mk]);
            const float foi = -0.5f * (zr[kk] - zr[mk]);
            re[k] = fer + wr_[k] * for_ - wi_[k] * foi;
            im[k] = fei + wr_[k] * foi + wi_[k] * for_;
        }
    }

    void inverse(const float* re, const float* im, float* out) {
        float* zr = zr_.data(); float* zi = zi_.data();
        for (size_t k = 0; k < m_; ++k) {
            const size_t mk = m_ - k;
            const float fer = 0.5f * (re[k] + re[mk]);
            const float fei = 0.5f * (im[k] - im[mk]);
            const float dr = 0.5f * (re[k] - re[mk]);
            const float di = 0.5f * (im[k] + im[mk]);
            const float for_ = dr * wr_[k] + di * wi_[k];
            const float foi = di * wr_[k] - dr * wi_[k];
            zr[k] = fer - foi;
            zi[k] = fei + for_;
        }
        cfft_.inverse(zr, zi);
        for (size_t k = 0; k < m_; ++k) { out[2 * k] = zr[k]; out[2 * k + 1] = zi[k]; }
    }

private:
    size_t n_ = 0, m_ = 0;
    ComplexFFT cfft_;
    AlignedArray<float> zr_, zi_;
    AlignedArray<float> wr_, wi_;
};

} // namespace llc
