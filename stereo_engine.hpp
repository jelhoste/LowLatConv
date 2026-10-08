// Stereo front-end of the convolver: maps an IR with 1, 2 or 4 channels and an input mode onto
// the path matrix of Convolver.
//
//  IR channels   Stereo input (L,R)                       Mono input (Sum = (L+R)/2, Left, Right)
//  1 (mono)      L->L h0 ; R->R h0                        m->L h0 ; m->R h0 (one convolution, duplicated)
//  2 (stereo)    L->L h0 ; R->R h1                        m->L h0 ; m->R h1
//  4 (true st.)  L->L h0 ; L->R h1 ; R->L h2 ; R->R h3    Sum: m->L (h0+h2), m->R (h1+h3)
//                                                         Left: m->L h0, m->R h1 ; Right: m->L h2, m->R h3
//  3 channels: the first two are used; more than 4: the first four.
#pragma once
#include "convolver.hpp"
#include "ir_processor.hpp"
#include <memory>

namespace llc {

enum class InputMode { Stereo, Sum, Left, Right };

class StereoEngine {
public:
    // Not real-time safe. Returns nullptr if the IR is unusable.
    static std::unique_ptr<StereoEngine> build(const IRBuffer& ir, InputMode mode, const ConvolverConfig& cfg = ConvolverConfig{}) {
        if (ir.ch.empty()) return nullptr;
        size_t nc = ir.ch.size(); if (nc == 3) nc = 2; if (nc > 4) nc = 4;
        size_t len = ir.ch[0].size();
        for (size_t c = 1; c < nc; ++c) len = std::min(len, ir.ch[c].size());
        if (len == 0) return nullptr;
        const float* h[4] = {nullptr, nullptr, nullptr, nullptr};
        for (size_t c = 0; c < nc; ++c) h[c] = ir.ch[c].data();

        auto e = std::unique_ptr<StereoEngine>(new StereoEngine());
        e->mode_ = mode;
        std::vector<PathSpec> paths;
        auto add = [&](int in, int out, const float* p) { PathSpec s; s.in = in; s.out = out; s.ir = p; s.length = len; paths.push_back(s); };
        int numIn = 1, numOut = 2;
        if (mode == InputMode::Stereo) {
            numIn = 2;
            if (nc == 1)      { add(0, 0, h[0]); add(1, 1, h[0]); }
            else if (nc == 2) { add(0, 0, h[0]); add(1, 1, h[1]); }
            else              { add(0, 0, h[0]); add(0, 1, h[1]); add(1, 0, h[2]); add(1, 1, h[3]); }
        } else if (nc == 1) {
            numOut = 1; e->dup_ = true; add(0, 0, h[0]);
        } else if (nc == 2) {
            add(0, 0, h[0]); add(0, 1, h[1]);
        } else if (mode == InputMode::Sum) {
            e->sum0_.resize(len); e->sum1_.resize(len);
            for (size_t i = 0; i < len; ++i) { e->sum0_[i] = h[0][i] + h[2][i]; e->sum1_[i] = h[1][i] + h[3][i]; }
            add(0, 0, e->sum0_.data()); add(0, 1, e->sum1_.data());
        } else if (mode == InputMode::Left) {
            add(0, 0, h[0]); add(0, 1, h[1]);
        } else {
            add(0, 0, h[2]); add(0, 1, h[3]);
        }
        if (!e->conv_.prepare(numIn, numOut, paths.data(), int(paths.size()), cfg)) return nullptr;
        e->mix_.resize(kChunk);
        return e;
    }

    bool ready() const { return conv_.ready(); }
    InputMode mode() const { return mode_; }
    size_t irLength() const { return conv_.irLength(); }
    size_t stageCount() const { return conv_.stageCount(); }
    int numInputs() const { return conv_.numIn(); }
    void reset() { conv_.reset(); }

    // Real-time safe. Any of the buffers may alias their input counterpart (in-place).
    void process(const float* inL, const float* inR, float* outL, float* outR, int n) {
        while (n > 0) {
            const int len = std::min(n, kChunk);
            const float* ins[2]; float* outs[2];
            if (mode_ == InputMode::Stereo) { ins[0] = inL; ins[1] = inR; }
            else if (mode_ == InputMode::Left) ins[0] = inL;
            else if (mode_ == InputMode::Right) ins[0] = inR;
            else { float* m = mix_.data(); for (int i = 0; i < len; ++i) m[i] = 0.5f * (inL[i] + inR[i]); ins[0] = m; }
            outs[0] = outL; outs[1] = outR;
            conv_.process(ins, outs, len);
            if (dup_) std::memcpy(outR, outL, size_t(len) * sizeof(float));
            inL += len; inR += len; outL += len; outR += len; n -= len;
        }
    }

private:
    StereoEngine() = default;
    static constexpr int kChunk = 1024;
    Convolver conv_;
    InputMode mode_ = InputMode::Stereo;
    bool dup_ = false;
    std::vector<float> sum0_, sum1_;
    std::vector<float> mix_;
};

} // namespace llc
