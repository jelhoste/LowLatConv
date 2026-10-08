// Live 3-band fully parametric equaliser + high-pass + low-pass.
//   chain: HP -> band1 -> band2 -> band3 -> LP
//   bands : bell / low shelf / high shelf, each with frequency, gain, Q
//   HP/LP : Butterworth, 12 / 24 / 48 dB per octave (1, 2 or 4 biquad sections)
// RBJ biquads, coefficients and state in double precision, TDF-II. Parameters are smoothed
// (in the log-frequency / dB / log-Q domains) and coefficients refreshed every 16 samples,
// so knob moves and automation are click-free. Zero latency, no allocation after prepare().
#pragma once
#include <algorithm>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cstring>

namespace llc {

enum class BandType { Bell, LowShelf, HighShelf };

struct EQBand {
    bool     enabled = false;
    BandType type    = BandType::Bell;
    double   freq    = 1000.0;   // Hz, 20 .. 20000
    double   gainDb  = 0.0;      // -18 .. +18
    double   q       = 1.0;      // 0.1 .. 10
};
struct EQPass {
    bool   enabled = false;
    double freq    = 100.0;      // HP: 20 .. 1000 Hz ; LP: 1000 .. 20000 Hz
    int    slope   = 12;         // dB / octave: 12, 24 or 48
};
struct EQParams {
    EQBand band[3];
    EQPass hp{false, 80.0, 12};
    EQPass lp{false, 8000.0, 12};
};

class ParametricEQ {
public:
    void prepare(double sampleRate) {
        fs_ = sampleRate > 1000.0 ? sampleRate : 48000.0;
        smoothCoef_ = 1.0 - std::exp(-double(kChunk) / (0.020 * fs_));   // ~20 ms time constant
        snap_ = true;
        reset();
    }
    void reset() {
        for (auto& b : bands_) b.sec.clearState();
        for (auto& p : {&hp_, &lp_}) for (auto& s : p->sec) s.clearState();
    }
    // `snap` = jump to the values immediately (initial state / preset load) instead of gliding.
    void setParams(const EQParams& p, bool snap = false) {
        for (int i = 0; i < 3; ++i) {
            Band& b = bands_[i];
            b.type = p.band[i].type;
            b.fT = clampd(p.band[i].freq, 20.0, 20000.0);
            b.qT = clampd(p.band[i].q, 0.1, 10.0);
            b.gT = p.band[i].enabled ? clampd(p.band[i].gainDb, -18.0, 18.0) : 0.0;
            if (snap || snap_) { b.f = b.fT; b.q = b.qT; b.g = b.gT; b.dirty = true; }
        }
        setPass(hp_, p.hp, true, snap || snap_);
        setPass(lp_, p.lp, false, snap || snap_);
        snap_ = false;
    }
    // r may be nullptr (mono). Real-time safe. In-place.
    void process(float* l, float* r, int n) {
        while (n > 0) {
            const int len = std::min(n, kChunk);
            advance();
            if (hp_.active)  for (int s = 0; s < hp_.sections; ++s)  runSection(hp_.sec[s], l, r, len);
            for (auto& b : bands_) if (b.active) runSection(b.sec, l, r, len);
            if (lp_.active)  for (int s = 0; s < lp_.sections; ++s)  runSection(lp_.sec[s], l, r, len);
            l += len; if (r) r += len; n -= len;
        }
    }

private:
    static constexpr int kChunk = 16;
    struct Coef { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    struct Section {
        Coef c; double z1[2] = {0, 0}, z2[2] = {0, 0};
        void clearState() { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }
    };
    struct Band { Section sec; BandType type = BandType::Bell; double f = 1000, g = 0, q = 1, fT = 1000, gT = 0, qT = 1; bool dirty = true, active = false; };
    struct Pass { Section sec[4]; bool isHP = true; bool enabled = false; double f = 0, fT = 0; int sections = 1, slope = 12; bool dirty = true, active = false; };

    static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : (v == v ? v : lo)); }
    double fmax() const { return 0.45 * fs_; }

    void setPass(Pass& p, const EQPass& in, bool isHP, bool snap) {
        const int slope = in.slope >= 48 ? 48 : (in.slope >= 24 ? 24 : 12);
        const int secs = slope / 12 == 1 ? 1 : (slope == 24 ? 2 : 4);
        p.isHP = isHP;
        const double lo = isHP ? 20.0 : 1000.0, hi = isHP ? 1000.0 : 20000.0;
        const double idle = isHP ? 5.0 : fmax();                    // "transparent" corner frequency
        const double target = in.enabled ? clampd(in.freq, lo, std::min(hi, fmax())) : idle;
        if (secs != p.sections || slope != p.slope) { for (auto& s : p.sec) s.clearState(); p.sections = secs; p.slope = slope; p.dirty = true; }
        if (in.enabled && !p.enabled) p.f = snap ? target : idle;     // glide in from the transparent end
        p.enabled = in.enabled;
        p.fT = target;
        if (snap) p.f = target;
        p.dirty = true;
        p.active = in.enabled || std::fabs(std::log(p.f / idle)) > 1e-3;
    }

    void advance() {
        const double k = smoothCoef_;
        for (auto& b : bands_) {
            const double df = std::log(b.fT / b.f), dq = std::log(b.qT / b.q), dg = b.gT - b.g;
            if (std::fabs(df) > 1e-5 || std::fabs(dq) > 1e-5 || std::fabs(dg) > 1e-5) {
                b.f *= std::exp(df * k); b.q *= std::exp(dq * k); b.g += dg * k; b.dirty = true;
                if (std::fabs(df) < 2e-5) b.f = b.fT;
                if (std::fabs(dq) < 2e-5) b.q = b.qT;
                if (std::fabs(dg) < 2e-4) b.g = b.gT;
            }
            if (b.dirty) { designBand(b); b.dirty = false; }
            b.active = std::fabs(b.g) > 1e-4;
        }
        advancePass(hp_); advancePass(lp_);
    }
    void advancePass(Pass& p) {
        const double idle = p.isHP ? 5.0 : fmax();
        const double df = std::log(p.fT / p.f);
        if (std::fabs(df) > 1e-5) { p.f *= std::exp(df * smoothCoef_); p.dirty = true; if (std::fabs(df) < 2e-5) p.f = p.fT; }
        if (p.dirty) { designPass(p); p.dirty = false; }
        p.active = p.enabled || std::fabs(std::log(p.f / idle)) > 1e-3;
    }

    void designBand(Band& b) {
        const double w0 = 2.0 * M_PI * clampd(b.f, 10.0, fmax()) / fs_;
        const double cw = std::cos(w0), sw = std::sin(w0), alpha = sw / (2.0 * b.q);
        const double A = std::pow(10.0, b.g / 40.0);
        double b0, b1, b2, a0, a1, a2;
        switch (b.type) {
        case BandType::Bell:
            b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A; break;
        case BandType::LowShelf: {
            const double t = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) - (A - 1) * cw + t); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - t);
            a0 = (A + 1) + (A - 1) * cw + t;       a1 = -2 * ((A - 1) + (A + 1) * cw);    a2 = (A + 1) + (A - 1) * cw - t; break; }
        default: {
            const double t = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) + (A - 1) * cw + t); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - t);
            a0 = (A + 1) - (A - 1) * cw + t;       a1 = 2 * ((A - 1) - (A + 1) * cw);     a2 = (A + 1) - (A - 1) * cw - t; break; }
        }
        b.sec.c = {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
    }
    void designPass(Pass& p) {
        const double w0 = 2.0 * M_PI * clampd(p.f, 1.0, fmax()) / fs_;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const int n = 2 * p.sections;                                 // Butterworth order
        for (int i = 0; i < p.sections; ++i) {
            const double Q = 1.0 / (2.0 * std::cos(M_PI * double(2 * i + 1) / double(2 * n)));
            const double alpha = sw / (2.0 * Q), a0 = 1 + alpha;
            double b0, b1, b2;
            if (p.isHP) { b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; }
            else        { b0 = (1 - cw) / 2; b1 = 1 - cw;    b2 = b0; }
            p.sec[i].c = {b0 / a0, b1 / a0, b2 / a0, -2 * cw / a0, (1 - alpha) / a0};
        }
    }
    static void runSection(Section& s, float* l, float* r, int n) {
        const Coef c = s.c;
        for (int ch = 0; ch < (r ? 2 : 1); ++ch) {
            float* x = ch ? r : l;
            double z1 = s.z1[ch], z2 = s.z2[ch];
            for (int i = 0; i < n; ++i) {
                const double in = x[i];
                const double out = c.b0 * in + z1;
                z1 = c.b1 * in - c.a1 * out + z2;
                z2 = c.b2 * in - c.a2 * out;
                x[i] = float(out);
            }
            s.z1[ch] = z1; s.z2[ch] = z2;
        }
    }

    double fs_ = 48000.0, smoothCoef_ = 0.1;
    bool snap_ = true;
    Band bands_[3];
    Pass hp_, lp_;
};

} // namespace llc
