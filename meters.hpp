// Level measurement for the plugin meters (IN = after the input gain, OUT = final output).
// Everything is integrated on the DSP side from 1 ms slices, so the UI only has to display / smooth:
//   peak     : sample peak, dBFS. INSTANT attack (a transient is reported by the block that contains it, whatever the
//              block size), then held for 50 ms (so no UI refresh can miss it) and released linearly in dB at a
//              configurable rate (5 .. 300 dB/s, default 24). Same model as the "meter decay (dB/s)" of REAPER.
//   peakMax  : exact largest |sample| since the last resetPeakMax() (peak watcher), dBFS
//   rms    : mean square over a configurable window (10 .. 3000 ms, default 300 ms), dB (a full-scale sine reads -3.01)
//   LUFS-M : ITU-R BS.1770 K-weighted loudness, 400 ms window, both channels summed (EBU "M")
//   LUFS-S : same, 3 s window (EBU "S")
// Values are floored at -120. Real-time safe, no allocation after prepare().
#pragma once
#include <algorithm>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cstdint>
#include <vector>

namespace llc {

class LevelMeters {
public:
    enum Sig { InL = 0, InR = 1, OutL = 2, OutR = 3 };
    struct Frame { float peak[4], peakMax[4], rms[4], lufsM[2], lufsS[2]; };   // lufs index: 0 = IN, 1 = OUT

    void prepare(double fs) {
        fs_ = fs >= 1000.0 ? fs : 48000.0;
        slice_ = std::max(1, int(std::lround(fs_ / 1000.0)));
        designK();
        for (auto& c : ch_) { c.ssRing.assign(kMaxSlices, 0.0); c.kRing.assign(kMaxSlices, 0.0); }
        setPeakDecayDbPerSec(decay_);
        setRmsWindowMs(rmsMs_);
        reset();
    }
    void setRmsWindowMs(double ms) {
        rmsMs_ = std::min(3000.0, std::max(10.0, ms == ms ? ms : 300.0));
        rmsSlices_ = int(std::lround(rmsMs_));
        for (auto& c : ch_) { double s = 0; for (int i = 0; i < rmsSlices_; ++i) s += c.ssRing[(c.pos + kMaxSlices - 1 - size_t(i)) % kMaxSlices]; c.rmsSum = s; }
    }
    void setPeakDecayDbPerSec(double dbPerSec) {
        decay_ = std::min(300.0, std::max(5.0, dbPerSec == dbPerSec ? dbPerSec : 24.0));
        decayPerSlice_ = float(decay_ * double(slice_) / fs_);
    }
    void resetPeakMax() { for (auto& c : ch_) c.maxAll = 0.0f; }
    void reset() {
        for (auto& c : ch_) {
            std::fill(c.ssRing.begin(), c.ssRing.end(), 0.0); std::fill(c.kRing.begin(), c.kRing.end(), 0.0);
            c.pos = 0; c.n = 0; c.peakAcc = 0; c.maxAll = 0; c.env = kFloor; c.pubMax = kFloor; c.hold = 0; c.ssAcc = 0; c.kAcc = 0; c.rmsSum = c.mSum = c.sSum = 0; c.f = Filt{};
        }
    }
    // Call once per channel group with the samples of that block (in-place buffers are fine: read only).
    void processIn(const float* l, const float* r, int n)  { run(ch_[InL], l, n); run(ch_[InR], r, n); }
    void processOut(const float* l, const float* r, int n) { run(ch_[OutL], l, n); run(ch_[OutR], r, n); }

    // Current values without consuming anything.
    Frame frame() const { return makeFrame(false); }
    // For the publisher (once per audio block): same values, but the peak also covers the highest level reached
    // since the previous call, so a transient is never lost even when the host uses very large blocks.
    Frame takeFrame() { const Frame f = makeFrame(true); for (auto& c : ch_) c.pubMax = c.env; return f; }

    // BS.1770 K-weighting biquads at the current rate (exposed for tests)
    struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    const Biquad& shelf() const { return shelf_; }
    const Biquad& highpass() const { return hp_; }

private:
    static constexpr int kMSlices = 400, kSSlices = 3000, kMaxSlices = 3000, kHoldSlices = 50;
    static constexpr float kFloor = -120.0f;
    struct Filt { double s1[2] = {0, 0}, s2[2] = {0, 0}; };
    struct Chan {
        std::vector<double> ssRing, kRing;
        size_t pos = 0; int n = 0; float peakAcc = 0, maxAll = 0, env = kFloor, pubMax = kFloor; int hold = 0; double ssAcc = 0, kAcc = 0, rmsSum = 0, mSum = 0, sSum = 0; Filt f;
    };
    static float floorDb(double v) { return float(std::max(-120.0, v)); }
    static float db20(float a) { return floorDb(20.0 * std::log10(std::max(1e-12f, a))); }
    static float db10(double p) { return floorDb(10.0 * std::log10(std::max(1e-12, p))); }

    void designK() {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        double K = std::tan(M_PI * f0 / fs_), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416), a0 = 1.0 + K / Q + K * K;
        shelf_ = {(Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
        const double f1 = 38.13547087602444, Q1 = 0.5003270373238773;
        K = std::tan(M_PI * f1 / fs_); a0 = 1.0 + K / Q1 + K * K;
        hp_ = {1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q1 + K * K) / a0};
    }
    static double bq(const Biquad& q, double x, double& z1, double& z2) { const double y = q.b0 * x + z1; z1 = q.b1 * x - q.a1 * y + z2; z2 = q.b2 * x - q.a2 * y; return y; }

    void run(Chan& c, const float* x, int n) {
        for (int i = 0; i < n; ++i) {
            double v = x[i]; if (!(v - v == 0.0)) v = 0.0;                    // NaN / Inf -> 0
            const float a = float(std::fabs(v)); if (a > c.peakAcc) c.peakAcc = a;
            if (a > c.maxAll) c.maxAll = a;
            c.ssAcc += v * v;
            const double k1 = bq(shelf_, v, c.f.s1[0], c.f.s2[0]);
            const double k2 = bq(hp_, k1, c.f.s1[1], c.f.s2[1]);
            c.kAcc += k2 * k2;
            if (++c.n == slice_) endSlice(c);
        }
    }
    void endSlice(Chan& c) {
        const size_t p = c.pos;
        {   // peak envelope: instant attack, 50 ms hold, then linear fall in dB
            const float sdb = db20(c.peakAcc);
            if (sdb >= c.env) { c.env = sdb; c.hold = kHoldSlices; }
            else if (c.hold > 0) --c.hold;
            else c.env = std::max(kFloor, std::max(sdb, c.env - decayPerSlice_));
            c.pubMax = std::max(c.pubMax, c.env);
        }
        auto old = [&](const std::vector<double>& r, int back) { return r[(p + kMaxSlices - size_t(back)) % kMaxSlices]; };   // slice leaving a window of `back` slices
        c.rmsSum += c.ssAcc - old(c.ssRing, rmsSlices_);
        c.mSum   += c.kAcc  - old(c.kRing, kMSlices);
        c.sSum   += c.kAcc  - old(c.kRing, kSSlices);
        c.rmsSum = std::max(0.0, c.rmsSum); c.mSum = std::max(0.0, c.mSum); c.sSum = std::max(0.0, c.sSum);
        c.ssRing[p] = c.ssAcc; c.kRing[p] = c.kAcc;
        c.pos = (p + 1) % kMaxSlices; c.n = 0; c.peakAcc = 0; c.ssAcc = 0; c.kAcc = 0;
    }

    Frame makeFrame(bool consume) const {
        Frame f;
        const double rmsSamples = double(rmsSlices_) * slice_, mSamples = double(kMSlices) * slice_, sSamples = double(kSSlices) * slice_;
        for (int i = 0; i < 4; ++i) {
            const Chan& c = ch_[i];
            float pk = std::max(c.env, db20(c.peakAcc));                      // the slice being filled counts immediately
            if (consume) pk = std::max(pk, c.pubMax);
            f.peak[i] = pk;
            f.peakMax[i] = db20(c.maxAll);
            f.rms[i] = db10(c.rmsSum / rmsSamples);
        }
        for (int g = 0; g < 2; ++g) {
            const Chan& a = ch_[2 * g]; const Chan& b = ch_[2 * g + 1];
            f.lufsM[g] = floorDb(-0.691 + 10.0 * std::log10(std::max(1e-12, (a.mSum + b.mSum) / mSamples)));
            f.lufsS[g] = floorDb(-0.691 + 10.0 * std::log10(std::max(1e-12, (a.sSum + b.sSum) / sSamples)));
        }
        return f;
    }

    double fs_ = 48000.0, rmsMs_ = 300.0, decay_ = 24.0; int slice_ = 48, rmsSlices_ = 300; float decayPerSlice_ = 0.024f;
    Biquad shelf_, hp_;
    Chan ch_[4];
};

} // namespace llc
