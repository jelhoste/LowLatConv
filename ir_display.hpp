// Data for the UI drawing of an impulse response (after all transformations):
//   * WAVE     : min / max per column (default 800 columns) for up to two traces (L, R)
//   * SPECTRUM : magnitude in dB on a logarithmic frequency axis (default 256 points, 20 Hz .. min(20 kHz, 0.45 fs))
// Computed from an already prepared IRBuffer (loader / UI worker thread, never the audio thread).
//
// Traces shown, per input mode (see stereo_engine.hpp):
//   1 IR channel : L = R = h0
//   2 IR channels: L = h0, R = h1
//   4 IR channels: Stereo input -> L = h0 (L->L), R = h3 (R->R);  Sum -> L = h0+h2, R = h1+h3;
//                  Left -> L = h0, R = h1;  Right -> L = h2, R = h3
#pragma once
#include "fft.hpp"
#include "ir_processor.hpp"
#include "stereo_engine.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace llc {

struct IRDisplay {
    uint32_t version = 0;                 // incremented by IRDisplayWorker for every new result
    bool     valid = false;
    int      traces = 0;                  // 1 or 2
    double   sampleRate = 48000.0;
    size_t   lengthSamples = 0;
    double   durationMs = 0.0;
    float    peak = 0.0f;                 // largest |sample| over the displayed traces
    double   fMin = 20.0, fMax = 20000.0; // frequency axis of `spectrumDb` (log spaced)
    std::vector<float> waveMin[2], waveMax[2];   // [trace][column]
    std::vector<float> spectrumDb[2];            // [trace][point], floor -120 dB
};

namespace irdisplay {

inline void traceChannels(const IRBuffer& ir, InputMode mode, std::vector<float> out[2], int& traces) {
    size_t nc = ir.ch.size(); if (nc == 3) nc = 2; if (nc > 4) nc = 4;
    size_t len = nc ? ir.ch[0].size() : 0; for (size_t c = 1; c < nc; ++c) len = std::min(len, ir.ch[c].size());
    traces = 0; out[0].clear(); out[1].clear();
    if (len == 0) return;
    auto cp = [&](size_t c) { return std::vector<float>(ir.ch[c].begin(), ir.ch[c].begin() + long(len)); };
    traces = 2;
    if (nc == 1) { out[0] = cp(0); out[1] = out[0]; traces = 1; }
    else if (nc == 2) { out[0] = cp(0); out[1] = cp(1); }
    else if (mode == InputMode::Stereo) { out[0] = cp(0); out[1] = cp(3); }
    else if (mode == InputMode::Left)   { out[0] = cp(0); out[1] = cp(1); }
    else if (mode == InputMode::Right)  { out[0] = cp(2); out[1] = cp(3); }
    else { out[0].resize(len); out[1].resize(len); for (size_t i = 0; i < len; ++i) { out[0][i] = ir.ch[0][i] + ir.ch[2][i]; out[1][i] = ir.ch[1][i] + ir.ch[3][i]; } }
}

} // namespace irdisplay

inline IRDisplay computeIRDisplay(const IRBuffer& ir, InputMode mode, int waveColumns = 800, int specPoints = 256) {
    IRDisplay d;
    std::vector<float> tr[2]; int traces = 0;
    irdisplay::traceChannels(ir, mode, tr, traces);
    if (traces == 0 || waveColumns < 2 || specPoints < 2) return d;
    const size_t len = tr[0].size();
    d.valid = true; d.traces = traces; d.sampleRate = ir.sampleRate; d.lengthSamples = len; d.durationMs = 1000.0 * double(len) / ir.sampleRate;
    d.fMax = std::min(20000.0, 0.45 * ir.sampleRate);
    for (int t = 0; t < traces; ++t) {
        d.waveMin[t].assign(size_t(waveColumns), 0.0f); d.waveMax[t].assign(size_t(waveColumns), 0.0f);
        for (int c = 0; c < waveColumns; ++c) {
            size_t a = size_t(double(c) * double(len) / waveColumns), b = size_t(double(c + 1) * double(len) / waveColumns);
            if (a >= len) a = len - 1;
            if (b <= a) b = a + 1;
            if (b > len) b = len;
            float mn = tr[t][a], mx = tr[t][a];
            for (size_t i = a; i < b; ++i) { mn = std::min(mn, tr[t][i]); mx = std::max(mx, tr[t][i]); }
            d.waveMin[t][size_t(c)] = mn; d.waveMax[t][size_t(c)] = mx;
            d.peak = std::max(d.peak, std::max(std::fabs(mn), std::fabs(mx)));
        }
        // spectrum: zero-padded real FFT, then log-spaced resampling (power average over wide bands, interpolation over narrow ones)
        size_t n = 8192; while (n < len) n *= 2; n = std::min<size_t>(n, size_t(1) << 21);
        std::vector<float> x(n, 0.0f); for (size_t i = 0; i < std::min(len, n); ++i) x[i] = tr[t][i];
        RealFFT fft; fft.init(n);
        const size_t bins = n / 2 + 1;
        std::vector<float> re(bins), im(bins); fft.forward(x.data(), re.data(), im.data());
        std::vector<double> mag(bins); for (size_t k = 0; k < bins; ++k) mag[k] = std::sqrt(double(re[k]) * re[k] + double(im[k]) * im[k]);
        const double binHz = ir.sampleRate / double(n), ratio = d.fMax / d.fMin;
        d.spectrumDb[t].assign(size_t(specPoints), -120.0f);
        for (int i = 0; i < specPoints; ++i) {
            const double fc = d.fMin * std::pow(ratio, double(i) / double(specPoints - 1));
            const double lo = d.fMin * std::pow(ratio, (double(i) - 0.5) / double(specPoints - 1)), hi = d.fMin * std::pow(ratio, (double(i) + 0.5) / double(specPoints - 1));
            const double blo = lo / binHz, bhi = hi / binHz; double m;
            if (bhi - blo < 1.0) { const double pos = std::min(fc / binHz, double(bins - 1) - 1e-9); const size_t k = size_t(pos); const double fr = pos - double(k); m = mag[k] + (mag[k + 1] - mag[k]) * fr; }
            else { size_t k0 = size_t(std::ceil(blo)), k1 = std::min(bins - 1, size_t(std::floor(bhi))); double s = 0; size_t cnt = 0; for (size_t k = k0; k <= k1; ++k) { s += mag[k] * mag[k]; ++cnt; } m = cnt ? std::sqrt(s / double(cnt)) : mag[std::min(bins - 1, size_t(fc / binHz))]; }
            d.spectrumDb[t][size_t(i)] = float(std::max(-120.0, 20.0 * std::log10(std::max(m, 1e-12))));
        }
    }
    if (traces == 1) { d.waveMin[1] = d.waveMin[0]; d.waveMax[1] = d.waveMax[0]; d.spectrumDb[1] = d.spectrumDb[0]; }
    return d;
}

// Background computation for the UI: request() from the UI thread, fetch() polled from the idle callback.
// `job` runs on the worker thread and must return the IR to display (typically loadWavFile + processIR).
class IRDisplayWorker {
public:
    using Job = std::function<bool(IRBuffer&, InputMode&)>;
    IRDisplayWorker() : th_([this] { loop(); }) {}
    ~IRDisplayWorker() { { std::lock_guard<std::mutex> lk(m_); quit_ = true; } cv_.notify_all(); th_.join(); }
    IRDisplayWorker(const IRDisplayWorker&) = delete; IRDisplayWorker& operator=(const IRDisplayWorker&) = delete;

    void request(Job job) { { std::lock_guard<std::mutex> lk(m_); job_ = std::move(job); pending_ = true; ++req_; } cv_.notify_all(); }
    // true when a result newer than `lastVersion` is available (copies it into `out`)
    bool fetch(IRDisplay& out, uint32_t lastVersion) { std::lock_guard<std::mutex> lk(m_); if (result_.version == lastVersion) return false; out = result_; return true; }

private:
    void loop() {
        for (;;) {
            Job job;
            { std::unique_lock<std::mutex> lk(m_); cv_.wait(lk, [&] { return pending_ || quit_; }); if (quit_) return; job = std::move(job_); pending_ = false; }
            IRBuffer ir; InputMode mode = InputMode::Sum; IRDisplay d;
            if (job && job(ir, mode)) d = computeIRDisplay(ir, mode);
            { std::lock_guard<std::mutex> lk(m_); if (pending_) continue;          // a newer request arrived meanwhile: drop this result
              d.version = ++ver_; result_ = std::move(d); }
        }
    }
    std::mutex m_; std::condition_variable cv_; Job job_; bool pending_ = false, quit_ = false; uint32_t req_ = 0, ver_ = 0; IRDisplay result_;
    std::thread th_;
};

} // namespace llc
