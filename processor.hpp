// Complete audio chain of the plugin (stereo in / stereo out, zero added latency):
//
//   in -> input gain --+--------------------------------------------> dry gain --+
//                      |                                                         +--> output gain -> out
//                      +-> pre-delay -> convolution -> EQ -> width -> wet gain --+
//
// * The convolution engine is swapped without clicks (LiveConvolver).
// * All gains, the width and the pre-delay are smoothed; the EQ smooths its own parameters.
// * With no IR loaded the plugin is a transparent bypass (input gain x output gain).
#pragma once
#include "denormals.hpp"
#include "eq.hpp"
#include "live_convolver.hpp"
#include "meters.hpp"
#include <vector>

namespace llc {

struct ProcessorParams {
    double inputGainDb  = 0.0;      // -60 .. +30
    double outputGainDb = 0.0;      // -60 .. +30
    double dryDb        = -120.0;   // <= -100 : off
    double wetDb        = 0.0;      // -60 .. +30
    double preDelayMs   = 0.0;      // 0 .. 2000 (wet path only)
    double width        = 1.0;      // 0 (mono) .. 2 (wide), wet path
    bool   eqEnabled    = true;
    EQParams eq;
};

// Stereo delay line; delay changes are applied by a short crossfade between two read positions.
class StereoDelay {
public:
    void prepare(double fs, double maxMs) {
        size_t n = 1; const size_t want = size_t(maxMs * 0.001 * fs) + 2; while (n < want) n *= 2;
        buf_[0].assign(n, 0.0f); buf_[1].assign(n, 0.0f); mask_ = n - 1; maxDelay_ = int(n) - 2;
        fade_ = std::max(16, int(0.020 * fs)); reset();
    }
    void reset() { std::fill(buf_[0].begin(), buf_[0].end(), 0.0f); std::fill(buf_[1].begin(), buf_[1].end(), 0.0f); w_ = 0; cur_ = next_ = target_ = 0; fading_ = false; pos_ = 0; }
    void setDelaySamples(int d, bool snap = false) { target_ = std::max(0, std::min(d, maxDelay_)); if (snap) { cur_ = target_; fading_ = false; } }
    int delaySamples() const { return cur_; }
    void process(const float* inL, const float* inR, float* outL, float* outR, int n) {
        for (int i = 0; i < n; ++i) {
            const float l = inL[i], r = inR[i];
            buf_[0][w_] = l; buf_[1][w_] = r;
            if (!fading_ && target_ != cur_) { next_ = target_; fading_ = true; pos_ = 0; }
            float yl, yr;
            if (!fading_) { const size_t ix = (w_ - size_t(cur_)) & mask_; yl = buf_[0][ix]; yr = buf_[1][ix]; }
            else {
                const size_t a = (w_ - size_t(cur_)) & mask_, b = (w_ - size_t(next_)) & mask_;
                const float g = float(pos_) / float(fade_);
                yl = buf_[0][a] + (buf_[0][b] - buf_[0][a]) * g; yr = buf_[1][a] + (buf_[1][b] - buf_[1][a]) * g;
                if (++pos_ >= fade_) { cur_ = next_; fading_ = false; }
            }
            outL[i] = yl; outR[i] = yr;
            w_ = (w_ + 1) & mask_;
        }
    }
private:
    std::vector<float> buf_[2];
    size_t mask_ = 0, w_ = 0; int maxDelay_ = 0, cur_ = 0, next_ = 0, target_ = 0, fade_ = 1, pos_ = 0; bool fading_ = false;
};

class ConvolutionProcessor {
public:
    ConvolutionProcessor() : live_(1024, true) {}

    void prepare(double sampleRate) {
        fs_ = sampleRate >= 1000.0 ? sampleRate : 48000.0;
        for (Smooth* sm : {&gIn_, &gOut_, &gDry_, &gWet_, &width_}) sm->ramp = std::max(16, int(0.020 * fs_));
        eq_.prepare(fs_);
        meters_.prepare(fs_);
        delay_.prepare(fs_, 2000.0);
        for (auto* v : {&xL_, &xR_, &wL_, &wR_}) v->assign(kChunk, 0.0f);
        snap_ = true;
        setParams(params_, true);
    }
    // Any thread that owns the audio callback's parameter snapshot (call it from the audio thread
    // at the start of a block, or from a thread-safe parameter queue).
    void setParams(const ProcessorParams& p, bool snap = false) {
        params_ = p;
        const bool s = snap || snap_;
        gIn_.set(dbToLin(clampd(p.inputGainDb, -60, 30)), s);
        gOut_.set(dbToLin(clampd(p.outputGainDb, -60, 30)), s);
        gDryUser_ = p.dryDb <= -100.0 ? 0.0 : dbToLin(clampd(p.dryDb, -100, 30));
        gWet_.set(p.wetDb <= -100.0 ? 0.0 : dbToLin(clampd(p.wetDb, -100, 30)), s);
        width_.set(clampd(p.width, 0.0, 2.0), s);
        delay_.setDelaySamples(int(std::lround(clampd(p.preDelayMs, 0.0, 2000.0) * 0.001 * fs_)), s);
        eq_.setParams(p.eq, s);
        gDry_.set((gDryUser_ > 0.0 || hasIR_) ? gDryUser_ : 1.0, s);
        snap_ = false;
    }

    // message thread
    void loadEngine(std::unique_ptr<StereoEngine> e) { live_.load(std::move(e)); }
    void unloadIR() { live_.unload(); }
    void collectRetired() { live_.collectRetired(); }
    int latencySamples() const { return 0; }
    void reset() { live_.reset(); delay_.reset(); eq_.reset(); meters_.reset(); }
    // Meters: IN = after the input gain, OUT = final output (after the output gain). Audio thread.
    void setMeterRmsMs(double ms) { meters_.setRmsWindowMs(ms); }
    void setMeterPeakDecay(double dbPerSec) { meters_.setPeakDecayDbPerSec(dbPerSec); }
    void resetMeterPeakMax() { meters_.resetPeakMax(); }
    LevelMeters::Frame meterFrame() const { return meters_.frame(); }          // peek (does not consume)
    LevelMeters::Frame takeMeterFrame() { return meters_.takeFrame(); }        // for the publisher, once per block

    // audio thread. Buffers may alias (in-place).
    void process(const float* inL, const float* inR, float* outL, float* outR, int n) {
        ScopedFlushDenormals ftz;
        const bool has = live_.hasIR();
        if (has != hasIR_) { hasIR_ = has; gDry_.set((gDryUser_ > 0.0 || hasIR_) ? gDryUser_ : 1.0, false); }
        while (n > 0) {
            const int len = std::min(n, kChunk);
            float* xL = xL_.data(); float* xR = xR_.data(); float* wL = wL_.data(); float* wR = wR_.data();
            for (int i = 0; i < len; ++i) { const float g = gIn_.next(); xL[i] = finiteOrZero(inL[i]) * g; xR[i] = finiteOrZero(inR[i]) * g; }
            meters_.processIn(xL, xR, len);
            delay_.process(xL, xR, wL, wR, len);
            live_.process(wL, wR, wL, wR, len);
            if (params_.eqEnabled) eq_.process(wL, wR, len);
            for (int i = 0; i < len; ++i) {
                const float w = width_.next();
                if (w != 1.0f) { const float m = 0.5f * (wL[i] + wR[i]), s = 0.5f * (wL[i] - wR[i]) * w; wL[i] = m + s; wR[i] = m - s; }
                const float gd = gDry_.next(), gw = gWet_.next(), go = gOut_.next();
                outL[i] = (xL[i] * gd + wL[i] * gw) * go;
                outR[i] = (xR[i] * gd + wR[i] * gw) * go;
            }
            meters_.processOut(outL, outR, len);
            inL += len; inR += len; outL += len; outR += len; n -= len;
        }
    }

    const ProcessorParams& params() const { return params_; }

private:
    // Linear ramp of fixed duration: reaches the target exactly (no endless exponential tail).
    struct Smooth {
        float cur = 1.0f, target = 1.0f, step = 0.0f; int left = 0;
        int ramp = 960;
        void set(double v, bool snap) {
            const float t = float(v);
            if (snap) { cur = target = t; left = 0; return; }
            if (t == target) return;
            target = t; left = ramp; step = (target - cur) / float(ramp);
        }
        float next() { if (left > 0) { cur += step; if (--left == 0) cur = target; } return cur; }
    };
    static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : (v == v ? v : lo)); }
    static float finiteOrZero(float v) { uint32_t b; std::memcpy(&b, &v, 4); return ((b & 0x7f800000u) == 0x7f800000u) ? 0.0f : v; }   // NaN / Inf -> 0
    static double dbToLin(double db) { return std::pow(10.0, db / 20.0); }
    static constexpr int kChunk = 512;
    double fs_ = 48000.0;
    bool snap_ = true, hasIR_ = false;
    double gDryUser_ = 0.0;
    ProcessorParams params_;
    Smooth gIn_, gOut_, gDry_, gWet_, width_;
    StereoDelay delay_;
    ParametricEQ eq_;
    LevelMeters meters_;
    LiveConvolver live_;
    std::vector<float> xL_, xR_, wL_, wR_;
};

} // namespace llc
