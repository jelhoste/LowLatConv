// Offline impulse-response preparation (loader thread, never the audio thread).
//   sample-rate conversion + stretch (windowed-sinc, Kaiser) -> automatic start trim -> begin / end
//   (both measured from the auto-trimmed start) -> automatic length (can only shorten further)
//   -> edge fades -> attack / decay envelope -> reverse -> normalisation.
// All channels are treated identically (same trims, same gain) so inter-channel timing and balance
// of stereo / true-stereo IRs are preserved.
#pragma once
#include <algorithm>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cstdint>
#include <cstring>
#include <vector>

namespace llc {

struct IRBuffer {
    double sampleRate = 48000.0;
    std::vector<std::vector<float>> ch;                 // ch[channel][sample]
    size_t length() const { return ch.empty() ? 0 : ch[0].size(); }
};

enum class NormMode { Off, Peak, Energy };

struct IROptions {
    double targetSampleRate = 48000.0;
    double stretch          = 1.0;      // 0.5 .. 2 (>1: longer and lower, like slowing a tape); clamped to 0.25 .. 4
    bool   autoTrimStart    = true;     // drop leading silence (built-in delay)
    double trimStartDb      = -60.0;    // threshold relative to the IR peak
    double beginMs          = 0.0;      // kept region starts here ...
    double endMs            = 0.0;      // ... and ends here (0 = keep to the end). Both are times on the axis of the
                                        //     (stretched, auto-trimmed) IR: 0 ms = first sample after the auto trim.
    bool   autoLength       = false;    // cut the tail below lengthDb
    double lengthDb         = -70.0;
    double attackMs         = 0.0;      // fade-in length of the envelope
    double attackShape      = 0.0;      // -1 (slow start) .. +1 (fast start)
    double decayDb          = 0.0;      // 0 .. 60: extra attenuation reached at the end (shortens the tail)
    bool   reverse          = false;
    NormMode norm           = NormMode::Energy;   // Energy: loudest channel has unit white-noise power gain
    double normTargetDb     = 0.0;
    double edgeFadeMs       = 2.0;      // fades applied where the IR was cut
    size_t maxLength        = size_t(1) << 20;
};

namespace irdetail {

inline double besselI0(double x) {
    double sum = 1.0, term = 1.0; const double q = x * x * 0.25;
    for (int k = 1; k < 60; ++k) { term *= q / (double(k) * double(k)); sum += term; if (term < 1e-18 * sum) break; }
    return sum;
}

// Output sample k is taken at input position k / ratio. Works for both up- and down-sampling.
inline std::vector<float> resample(const std::vector<float>& in, double ratio) {
    if (in.empty()) return {};
    if (std::fabs(ratio - 1.0) < 1e-12) return in;
    const size_t outN = std::max<size_t>(1, size_t(std::ceil(double(in.size()) * ratio)));
    const double fc = std::min(1.0, ratio) * 0.985;               // cutoff relative to the input Nyquist
    const int lobes = 32;
    const double halfW = double(lobes) / fc;                       // kernel half-width in input samples
    const int res = 512;                                           // table steps per input sample
    const int half = int(std::ceil(halfW));
    const size_t tn = size_t(half) * res + 2;
    std::vector<double> tab(tn);
    const double beta = 9.0, i0b = besselI0(beta);
    for (size_t i = 0; i < tn; ++i) {
        const double x = double(i) / res;
        const double u = x / halfW;
        const double w = u >= 1.0 ? 0.0 : besselI0(beta * std::sqrt(1.0 - u * u)) / i0b;
        const double a = M_PI * fc * x;
        tab[i] = fc * (x == 0.0 ? 1.0 : std::sin(a) / a) * w;
    }
    std::vector<float> out(outN);
    const long n = long(in.size());
    for (size_t k = 0; k < outN; ++k) {
        const double t = double(k) / ratio;
        const long j0 = long(std::floor(t)) - half + 1, j1 = long(std::floor(t)) + half;
        double acc = 0.0;
        for (long j = std::max(0L, j0); j <= std::min(n - 1, j1); ++j) {
            const double d = std::fabs(double(j) - t) * res;
            const size_t di = size_t(d); const double fr = d - double(di);
            if (di + 1 >= tn) continue;
            acc += double(in[size_t(j)]) * (tab[di] + (tab[di + 1] - tab[di]) * fr);
        }
        out[k] = float(acc);
    }
    return out;
}

inline float fin(float v) { uint32_t b; std::memcpy(&b, &v, 4); return ((b & 0x7f800000u) == 0x7f800000u) ? 0.0f : v; }

} // namespace irdetail

// Returns false if the input is unusable (empty, silent, invalid sample rate).
inline bool processIR(const IRBuffer& src, const IROptions& o, IRBuffer& out) {
    out = IRBuffer{};
    if (src.ch.empty() || src.sampleRate < 1000.0 || o.targetSampleRate < 1000.0) return false;
    size_t n0 = 0; for (auto& c : src.ch) n0 = std::max(n0, c.size());
    if (n0 == 0) return false;

    IRBuffer b; b.sampleRate = o.targetSampleRate; b.ch.resize(src.ch.size());
    for (size_t c = 0; c < src.ch.size(); ++c) {
        std::vector<float> v(n0, 0.0f);
        for (size_t i = 0; i < src.ch[c].size(); ++i) v[i] = irdetail::fin(src.ch[c][i]);
        b.ch[c] = std::move(v);
    }
    const double stretch = std::min(4.0, std::max(0.25, o.stretch == o.stretch ? o.stretch : 1.0));
    const double ratio = (o.targetSampleRate / src.sampleRate) * stretch;
    if (std::fabs(ratio - 1.0) > 1e-12)
        for (auto& c : b.ch) c = irdetail::resample(c, ratio);

    auto peakOf = [&](size_t from, size_t to) { double p = 0; for (auto& c : b.ch) for (size_t i = from; i < to && i < c.size(); ++i) p = std::max(p, double(std::fabs(c[i]))); return p; };
    const double fs = o.targetSampleRate;
    auto msToSamples = [&](double ms) { return size_t(std::max(0.0, ms) * 0.001 * fs + 0.5); };
    size_t len = b.length(), start = 0, end = len;
    bool cutEnd = false, cutStart = false;

    if (o.autoTrimStart) {
        const double thr = peakOf(0, len) * std::pow(10.0, o.trimStartDb / 20.0);
        if (thr > 0) for (size_t i = 0; i < len; ++i) { bool hit = false; for (auto& c : b.ch) if (std::fabs(c[i]) >= thr) hit = true; if (hit) { start = i; break; } }
    }
    const size_t origin = start;                                     // time axis of Begin / End
    if (o.beginMs > 0) { start = origin + msToSamples(o.beginMs); cutStart = true; }
    if (start >= len) return false;
    if (o.endMs > 0) { const size_t e = origin + msToSamples(o.endMs); if (e < end) { end = std::max(e, start + 1); cutEnd = true; } }
    if (o.autoLength) {
        const double thr = peakOf(start, end) * std::pow(10.0, o.lengthDb / 20.0);
        if (thr > 0) {
            size_t last = start;
            for (size_t blk = (end - 1) / 64 * 64 + 64; blk >= 64; blk -= 64) {          // scan blocks backwards
                const size_t from = blk - 64, to = std::min(blk, end);
                if (from >= to) continue;
                if (peakOf(from, to) >= thr) { last = to; break; }
            }
            if (last > start && last < end) { end = last; cutEnd = true; }
        }
    }
    if (end - start > o.maxLength) { end = start + o.maxLength; cutEnd = true; }
    for (auto& c : b.ch) c = std::vector<float>(c.begin() + long(start), c.begin() + long(end));
    len = b.length();

    const size_t fade = std::min(msToSamples(o.edgeFadeMs), len / 4);
    if (fade > 1) {
        for (auto& c : b.ch) {
            if (cutStart) for (size_t i = 0; i < fade; ++i) c[i] *= float(0.5 - 0.5 * std::cos(M_PI * double(i) / double(fade)));
            if (cutEnd)   for (size_t i = 0; i < fade; ++i) c[len - 1 - i] *= float(0.5 - 0.5 * std::cos(M_PI * double(i) / double(fade)));
        }
    }
    const size_t na = msToSamples(o.attackMs);
    const double p = std::pow(4.0, -std::max(-1.0, std::min(1.0, o.attackShape)));
    const double decay = std::max(0.0, std::min(60.0, o.decayDb));
    if (na > 1 || decay > 0)
        for (auto& c : b.ch)
            for (size_t i = 0; i < len; ++i) {
                double g = 1.0;
                if (i < na) g *= std::pow(double(i) / double(na), p);
                if (decay > 0 && len > 1) g *= std::pow(10.0, -decay * double(i) / double(len - 1) / 20.0);
                c[i] = float(c[i] * g);
            }
    if (o.reverse) for (auto& c : b.ch) std::reverse(c.begin(), c.end());

    if (o.norm != NormMode::Off) {
        double ref = 0;
        if (o.norm == NormMode::Peak) ref = peakOf(0, len);
        else for (auto& c : b.ch) { double e = 0; for (float v : c) e += double(v) * v; ref = std::max(ref, std::sqrt(e)); }
        if (ref <= 1e-20) return false;
        const float g = float(std::pow(10.0, o.normTargetDb / 20.0) / ref);
        for (auto& c : b.ch) for (auto& v : c) v *= g;
    } else if (peakOf(0, len) <= 0) return false;
    out = std::move(b);
    return true;
}

} // namespace llc
