// FFT backend selection for the convolver.
//   -DLLC_USE_PFFFT : PFFFT (SIMD, BSD-like licence) in third_party/pffft  [recommended]
//   (default)       : built-in portable radix-2 FFT (fft.hpp), no external dependency
//
// A backend exposes an *opaque* spectrum layout of specFloats() floats:
//   init(n)                 : prepare size n (power of two); false if unsupported
//   forward(in, spec)       : real input (n floats) -> spectrum
//   inverse(spec, out)      : spectrum -> n real samples, scaled by 1/inverseScale() (unnormalised)
//   mulAcc(a, b, acc)       : acc += a * b   (complex, per bin)
//   inverseScale()          : factor that makes inverse(forward(x)) == x
// Buffers passed in must be 64-byte aligned (use AlignedArray).
#pragma once
#include "fft.hpp"
#include <cstring>

#ifdef LLC_USE_PFFFT
#ifndef PFFFT_STATIC_DEFINE
#define PFFFT_STATIC_DEFINE            // PFFFT is linked statically (otherwise its header requests a DLL import on Windows)
#endif
extern "C" {
#include "pffft.h"
}
#endif

namespace llc {

class BuiltinSpectralFFT {
public:
    static const char* name() { return "builtin radix-2"; }
    static int minSize() { return 8; }
    bool init(size_t n) {
        if (n < 8 || (n & (n - 1))) return false;
        n_ = n; bins_ = n / 2 + 1; fft_.init(n); return true;
    }
    size_t size() const { return n_; }
    size_t specFloats() const { return 2 * bins_; }
    float inverseScale() const { return fft_.inverseScale(); }
    void forward(const float* in, float* spec) { fft_.forward(in, spec, spec + bins_); }
    void inverse(const float* spec, float* out) { fft_.inverse(spec, spec + bins_, out); }
    void mulAcc(const float* a, const float* b, float* acc) const {
        const float* __restrict ar = a;  const float* __restrict ai = a + bins_;
        const float* __restrict br = b;  const float* __restrict bi = b + bins_;
        float* __restrict cr = acc;      float* __restrict ci = acc + bins_;
        for (size_t k = 0; k < bins_; ++k) {
            cr[k] += ar[k] * br[k] - ai[k] * bi[k];
            ci[k] += ar[k] * bi[k] + ai[k] * br[k];
        }
    }
private:
    size_t n_ = 0, bins_ = 0;
    RealFFT fft_;
};

#ifdef LLC_USE_PFFFT
class PffftSpectralFFT {
public:
    static const char* name() { return "PFFFT"; }
    static int minSize() { return pffft_min_fft_size(PFFFT_REAL); }
    PffftSpectralFFT() = default;
    PffftSpectralFFT(const PffftSpectralFFT&) = delete;
    PffftSpectralFFT& operator=(const PffftSpectralFFT&) = delete;
    PffftSpectralFFT(PffftSpectralFFT&& o) noexcept : setup_(o.setup_), n_(o.n_), work_(std::move(o.work_)) { o.setup_ = nullptr; o.n_ = 0; }
    PffftSpectralFFT& operator=(PffftSpectralFFT&& o) noexcept {
        if (this != &o) { release(); setup_ = o.setup_; n_ = o.n_; work_ = std::move(o.work_); o.setup_ = nullptr; o.n_ = 0; }
        return *this;
    }
    ~PffftSpectralFFT() { release(); }

    bool init(size_t n) {
        release();
        if (n < size_t(minSize()) || !pffft_is_power_of_two(int(n))) return false;
        setup_ = pffft_new_setup(int(n), PFFFT_REAL);
        if (!setup_) return false;
        n_ = n; work_.resize(n);
        return true;
    }
    size_t size() const { return n_; }
    size_t specFloats() const { return n_; }
    float inverseScale() const { return 1.0f / float(n_); }
    void forward(const float* in, float* spec) { pffft_transform(setup_, in, spec, work_.data(), PFFFT_FORWARD); }
    void inverse(const float* spec, float* out) { pffft_transform(setup_, spec, out, work_.data(), PFFFT_BACKWARD); }
    void mulAcc(const float* a, const float* b, float* acc) const { pffft_zconvolve_accumulate(setup_, a, b, acc, 1.0f); }
private:
    void release() { if (setup_) pffft_destroy_setup(setup_); setup_ = nullptr; n_ = 0; }
    PFFFT_Setup* setup_ = nullptr;
    size_t n_ = 0;
    AlignedArray<float> work_;
};
using SpectralFFT = PffftSpectralFFT;
#else
using SpectralFFT = BuiltinSpectralFFT;
#endif

} // namespace llc
