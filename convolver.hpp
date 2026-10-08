// Zero-added-latency convolution engine with an arbitrary matrix of filter paths.
//
//  * `numIn` input channels, `numOut` output channels, any number of paths (in -> out, own IR).
//    Mono = one path; stereo = two; true stereo = four. Paths that share an input share its FFT,
//    paths that share an output share its inverse FFT (true stereo costs 2+2 FFTs, not 4+4).
//  * Head: the first `headSize` taps of every path are computed in the time domain, sample by sample.
//  * Tail: remaining taps are split into non-uniform FFT partitions (Gardner scheme): hop sizes grow
//    geometrically from `headSize` up to `maxPartition`.
//  * Works with ANY host block size (1..N); the internal grid is independent of it.
//  * process() never allocates, locks, or throws. prepare() allocates (call it off the audio thread,
//    then hand the object to the audio thread).
#pragma once
#include "aligned.hpp"
#include "fft_backend.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace llc {

struct ConvolverConfig {
    int  headSize        = 64;     // power of two, 16..1024: direct-form taps == smallest hop
    int  maxPartition    = 1024;   // power of two >= headSize, <= 32768: largest FFT hop (smaller = lower worst-case spike)
    bool sanitizeInput   = true;   // replace NaN/Inf input samples by 0 (protects the FFT history)
    size_t maxIRLength   = size_t(1) << 24;
};

struct PathSpec {
    int in = 0, out = 0;           // channel indices
    const float* ir = nullptr;     // impulse response (copied/transformed by prepare)
    size_t length = 0;
};

class Convolver {
public:
    // Not real-time safe. Mono convenience: one path, one input, one output.
    bool prepare(const float* ir, size_t irLength, const ConvolverConfig& cfg = {}) {
        PathSpec p; p.ir = ir; p.length = irLength;
        return prepare(1, 1, &p, 1, cfg);
    }

    // Not real-time safe. Returns false if the description is unusable.
    bool prepare(int numIn, int numOut, const PathSpec* paths, int numPaths, const ConvolverConfig& cfg = {}) {
        ready_ = false; stages_.clear(); paths_.clear(); xh_.clear();
        if (numIn < 1 || numIn > 4 || numOut < 1 || numOut > 4 || !paths || numPaths < 1 || numPaths > 16) return false;
        size_t maxLen = 0;
        for (int k = 0; k < numPaths; ++k) {
            const PathSpec& p = paths[k];
            if (!p.ir || p.length == 0 || p.in < 0 || p.in >= numIn || p.out < 0 || p.out >= numOut) return false;
            maxLen = std::max(maxLen, std::min(p.length, cfg.maxIRLength));
        }
        numIn_ = numIn; numOut_ = numOut; irLength_ = maxLen;

        int lo = 16; while (lo * 2 < SpectralFFT::minSize()) lo *= 2;     // 2*headSize is the smallest FFT
        H_ = clampPow2(cfg.headSize, lo, 1024);
        const int maxB = std::max(H_, clampPow2(cfg.maxPartition, 16, 32768));
        sanitize_ = cfg.sanitizeInput;

        paths_.resize(size_t(numPaths));
        for (int k = 0; k < numPaths; ++k) {
            Path& q = paths_[size_t(k)];
            q.in = paths[k].in; q.out = paths[k].out;
            q.len = std::min(paths[k].length, cfg.maxIRLength);
            q.hrev.resize(size_t(H_));
            for (int t = 0; t < H_; ++t) {                                  // reversed -> contiguous dot product
                const size_t i = size_t(H_ - 1 - t);
                q.hrev[size_t(t)] = i < q.len ? finiteOrZero(paths[k].ir[i]) : 0.0f;
            }
        }
        cap_ = size_t(H_ - 1) + 8192;
        xh_.resize(size_t(numIn));
        for (auto& x : xh_) x.resize(cap_);
        hpos_ = size_t(H_ - 1);

        size_t a = size_t(H_);
        int maxHop = H_;
        while (a < maxLen) {
            const int B = static_cast<int>(std::min<size_t>(size_t(maxB), pow2floor(a)));
            const size_t np = (B < maxB) ? 1 : (maxLen - a + size_t(B) - 1) / size_t(B);
            stages_.emplace_back();
            Stage& s = stages_.back();
            s.B = B; s.c = static_cast<int>(a / size_t(B)); s.np = static_cast<int>(np);
            s.K = size_t(s.c) + np - 1;
            if (!s.fft.init(size_t(2 * B))) { stages_.clear(); return false; }
            const size_t sf = s.fft.specFloats();
            s.sf = sf;
            s.inUsed.assign(size_t(numIn), 0); s.outUsed.assign(size_t(numOut), 0);
            s.nPart.assign(size_t(numPaths), 0);
            for (int k = 0; k < numPaths; ++k) {
                const size_t len = paths_[size_t(k)].len;
                if (len > a) {
                    s.nPart[size_t(k)] = int(std::min<size_t>(np, (len - a + size_t(B) - 1) / size_t(B)));
                    s.inUsed[size_t(paths_[size_t(k)].in)] = 1; s.outUsed[size_t(paths_[size_t(k)].out)] = 1;
                }
            }
            s.win.resize(size_t(numIn)); s.fdl.resize(size_t(numIn));
            for (int i = 0; i < numIn; ++i) if (s.inUsed[size_t(i)]) { s.win[size_t(i)].resize(size_t(2 * B)); s.fdl[size_t(i)].resize(s.K * sf); }
            s.time.resize(size_t(numOut)); s.out.resize(size_t(numOut)); s.acc.resize(size_t(numOut));
            for (int o = 0; o < numOut; ++o) { s.out[size_t(o)].resize(size_t(B)); if (s.outUsed[size_t(o)]) { s.time[size_t(o)].resize(size_t(2 * B)); s.acc[size_t(o)].resize(sf); } }
            s.h.resize(size_t(numPaths) * np * sf);
            // Filter spectra, pre-scaled so the unnormalised inverse yields the true result.
            AlignedArray<float> tmp(size_t(2 * B));
            const float scale = s.fft.inverseScale();
            for (int k = 0; k < numPaths; ++k)
                for (int p = 0; p < s.nPart[size_t(k)]; ++p) {
                    tmp.zero();
                    const size_t start = a + size_t(p) * size_t(B);
                    for (size_t i = 0; i < size_t(B) && start + i < paths_[size_t(k)].len; ++i) tmp[i] = finiteOrZero(paths[k].ir[start + i]);
                    float* hp = s.h.data() + (size_t(k) * np + size_t(p)) * sf;
                    s.fft.forward(tmp.data(), hp);
                    for (size_t j = 0; j < sf; ++j) hp[j] *= scale;
                }
            a += np * size_t(B);
            maxHop = std::max(maxHop, B);
        }
        phaseMask_ = static_cast<uint32_t>(maxHop - 1);
        reset();
        ready_ = true;
        return true;
    }

    bool ready() const { return ready_; }
    size_t irLength() const { return irLength_; }
    int numIn() const { return numIn_; }
    int numOut() const { return numOut_; }
    int latencySamples() const { return 0; }
    size_t stageCount() const { return stages_.size(); }
    static const char* fftName() { return SpectralFFT::name(); }

    // Real-time safe. Clears all state (silence).
    void reset() {
        for (auto& x : xh_) x.zero();
        hpos_ = size_t(H_ - 1);
        phase_ = 0;
        for (auto& s : stages_) {
            for (auto& w : s.win) w.zero();
            for (auto& f : s.fdl) f.zero();
            for (auto& o : s.out) o.zero();
            for (auto& t : s.time) t.zero();
            for (auto& a : s.acc) a.zero();
            s.fdlPos = 0;
        }
    }

    // Mono convenience (engine must have been prepared with 1 in / 1 out). in == out allowed.
    void process(const float* in, float* out, int n) {
        if (!ready_ || numIn_ != 1 || numOut_ != 1) { if (n > 0) std::memset(out, 0, size_t(n) * sizeof(float)); return; }
        const float* ins[1] = {in}; float* outs[1] = {out};
        process(ins, outs, n);
    }

    // Real-time safe. ins[i] / outs[o] may alias (in-place). n may be any value >= 0.
    void process(const float* const* ins, float* const* outs, int n) {
        if (!ready_) { for (int o = 0; o < numOut_; ++o) if (n > 0) std::memset(outs[o], 0, size_t(n) * sizeof(float)); return; }
        const uint32_t hmask = uint32_t(H_ - 1);
        float* o_[4]; const float* i_[4];
        for (int i = 0; i < numIn_; ++i) i_[i] = ins[i];
        for (int o = 0; o < numOut_; ++o) o_[o] = outs[o];
        while (n > 0) {
            const int len = std::min(n, int(H_ - (phase_ & hmask)));

            // 1. Append inputs to the head histories and to the partition windows (all inputs are
            //    fully consumed before any output sample is written, so in-place is safe).
            if (hpos_ + size_t(len) > cap_) {
                for (auto& x : xh_) std::memmove(x.data(), x.data() + (hpos_ - size_t(H_ - 1)), size_t(H_ - 1) * sizeof(float));
                hpos_ = size_t(H_ - 1);
            }
            for (int i = 0; i < numIn_; ++i) {
                float* xin = xh_[size_t(i)].data() + hpos_;
                if (sanitize_) for (int j = 0; j < len; ++j) xin[j] = finiteOrZero(i_[i][j]);
                else std::memcpy(xin, i_[i], size_t(len) * sizeof(float));
                for (auto& s : stages_)
                    if (s.inUsed[size_t(i)]) std::memcpy(s.win[size_t(i)].data() + s.B + (phase_ & uint32_t(s.B - 1)), xin, size_t(len) * sizeof(float));
            }

            // 2. Outputs: head (direct form) of every path ...
            for (int o = 0; o < numOut_; ++o) std::memset(o_[o], 0, size_t(len) * sizeof(float));
            const size_t b0 = hpos_ - size_t(H_ - 1);
            for (auto& q : paths_) {
                const float* base = xh_[size_t(q.in)].data() + b0;
                float* __restrict dst = o_[q.out];
                for (int j = 0; j < len; ++j) dst[j] += dot(q.hrev.data(), base + j, H_);
            }
            hpos_ += size_t(len);
            // ... plus the pre-computed output block of every partition stage.
            for (auto& s : stages_)
                for (int o = 0; o < numOut_; ++o) {
                    if (!s.outUsed[size_t(o)]) continue;
                    const float* __restrict src = s.out[size_t(o)].data() + (phase_ & uint32_t(s.B - 1));
                    float* __restrict dst = o_[o];
                    for (int j = 0; j < len; ++j) dst[j] += src[j];
                }

            // 3. Advance; run every partition whose block just completed.
            phase_ = (phase_ + uint32_t(len)) & phaseMask_;
            for (int i = 0; i < numIn_; ++i) i_[i] += len;
            for (int o = 0; o < numOut_; ++o) o_[o] += len;
            n -= len;
            if ((phase_ & hmask) == 0)
                for (auto& s : stages_)
                    if ((phase_ & uint32_t(s.B - 1)) == 0) runStage(s);
        }
    }

private:
    struct Path { int in = 0, out = 0; size_t len = 0; AlignedArray<float> hrev; };
    struct Stage {
        int B = 0, c = 0, np = 0;
        size_t K = 0, sf = 0;
        int fdlPos = 0;
        SpectralFFT fft;
        std::vector<char> inUsed, outUsed;
        std::vector<int> nPart;                                   // per path: partitions holding non-zero taps
        std::vector<AlignedArray<float>> win, fdl;                // per input
        std::vector<AlignedArray<float>> time, out, acc;          // per output
        AlignedArray<float> h;                                    // [path][partition][sf]
    };

    static float finiteOrZero(float v) {
        uint32_t b; std::memcpy(&b, &v, 4);
        return ((b & 0x7f800000u) == 0x7f800000u) ? 0.0f : v;   // robust even under -ffast-math
    }
    static int clampPow2(int v, int lo, int hi) {
        v = std::max(lo, std::min(hi, v));
        int p = lo; while (p * 2 <= v) p *= 2;
        return p;
    }
    static size_t pow2floor(size_t v) { size_t p = 1; while (p * 2 <= v) p *= 2; return p; }

    static float dot(const float* __restrict a, const float* __restrict b, int n) {
        float acc[16] = {0};
        for (int i = 0; i < n; i += 16)
            for (int l = 0; l < 16; ++l) acc[l] += a[i + l] * b[i + l];
        float s = 0;
        for (int l = 0; l < 16; ++l) s += acc[l];
        return s;
    }

    void runStage(Stage& s) {
        const size_t sf = s.sf;
        s.fdlPos = (s.fdlPos + 1) % int(s.K);
        for (int i = 0; i < numIn_; ++i)
            if (s.inUsed[size_t(i)]) s.fft.forward(s.win[size_t(i)].data(), s.fdl[size_t(i)].data() + size_t(s.fdlPos) * sf);
        for (int o = 0; o < numOut_; ++o) {
            if (!s.outUsed[size_t(o)]) continue;
            s.acc[size_t(o)].zero();
            for (size_t k = 0; k < paths_.size(); ++k) {
                if (paths_[k].out != o) continue;
                const float* fdl = s.fdl[size_t(paths_[k].in)].data();
                for (int p = 0; p < s.nPart[k]; ++p) {
                    const int age = s.c + p - 1;                         // 0 <= age < K
                    const size_t idx = size_t((s.fdlPos + int(s.K) - age) % int(s.K));
                    s.fft.mulAcc(fdl + idx * sf, s.h.data() + (k * size_t(s.np) + size_t(p)) * sf, s.acc[size_t(o)].data());
                }
            }
            s.fft.inverse(s.acc[size_t(o)].data(), s.time[size_t(o)].data());
            std::memcpy(s.out[size_t(o)].data(), s.time[size_t(o)].data() + s.B, size_t(s.B) * sizeof(float));
        }
        for (int i = 0; i < numIn_; ++i)
            if (s.inUsed[size_t(i)]) std::memcpy(s.win[size_t(i)].data(), s.win[size_t(i)].data() + s.B, size_t(s.B) * sizeof(float));
    }

    bool ready_ = false, sanitize_ = true;
    int H_ = 64, numIn_ = 1, numOut_ = 1;
    size_t irLength_ = 0, cap_ = 0, hpos_ = 0;
    uint32_t phase_ = 0, phaseMask_ = 63;
    std::vector<Path> paths_;
    std::vector<AlignedArray<float>> xh_;
    std::vector<Stage> stages_;
};

} // namespace llc
