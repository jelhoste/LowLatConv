#include "processor.hpp"
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
using namespace llc;
static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
typedef std::vector<float> V;

static std::unique_ptr<StereoEngine> delta(float g, int channels = 1) {
    IRBuffer ir; ir.ch.assign(size_t(channels), V(1, g)); return StereoEngine::build(ir, InputMode::Stereo);
}
static void runAll(ConvolutionProcessor& p, const V& L, const V& R, V& yL, V& yR, int block) {
    yL.assign(L.size(), 0.f); yR.assign(L.size(), 0.f);
    for (size_t pos = 0; pos < L.size();) { const int n = int(std::min<size_t>(size_t(block), L.size() - pos)); p.process(L.data() + pos, R.data() + pos, yL.data() + pos, yR.data() + pos, n); pos += size_t(n); }
}
static double rms(const V& v, size_t from, size_t to) { double s = 0; for (size_t i = from; i < to; ++i) s += double(v[i]) * v[i]; return std::sqrt(s / double(to - from)); }
static V noise(size_t n, unsigned seed) { std::mt19937 r(seed); std::uniform_real_distribution<float> d(-0.5f, 0.5f); V v(n); for (auto& x : v) x = d(r); return v; }
static V sine(size_t n, double f, double fs = 48000.0, double a = 0.5) { V v(n); for (size_t i = 0; i < n; ++i) v[i] = float(a * std::sin(2 * M_PI * f * double(i) / fs)); return v; }
static ConvolutionProcessor* make() { auto* p = new ConvolutionProcessor(); p->prepare(48000.0); return p; }

static void testBasics() {
    std::printf("[chain] bypass without IR, gains, dry/wet\n");
    V L = noise(20000, 1), R = noise(20000, 2), yL, yR;
    { std::unique_ptr<ConvolutionProcessor> p(make()); runAll(*p, L, R, yL, yR, 100);
      CHECK(yL == L && yR == R, "without IR the plugin must be a bit-exact bypass"); }
    { std::unique_ptr<ConvolutionProcessor> p(make()); ProcessorParams q; q.inputGainDb = 6; q.outputGainDb = 6; p->setParams(q, true); runAll(*p, L, R, yL, yR, 77);
      const double g = std::pow(10.0, 12.0 / 20.0); double e = 0; for (size_t i = 0; i < L.size(); ++i) e = std::max(e, std::fabs(double(yL[i]) - g * L[i]));
      CHECK(e < 1e-4, "bypass with +12 dB total gain: err %g", e); }
    { std::unique_ptr<ConvolutionProcessor> p(make()); p->loadEngine(delta(0.5f)); ProcessorParams q; p->setParams(q, true);
      V a = noise(30000, 3), b = noise(30000, 4); runAll(*p, a, b, yL, yR, 64);
      double e = 0; for (size_t i = 5000; i < a.size(); ++i) e = std::max(e, std::fabs(double(yL[i]) - 0.5 * a[i]));
      CHECK(e < 1e-6, "wet only, IR = 0.5: err %g", e);
      q.dryDb = -6; q.wetDb = -6; p->setParams(q); runAll(*p, a, b, yL, yR, 64);
      const double g = std::pow(10.0, -6.0 / 20.0); e = 0; for (size_t i = 15000; i < a.size(); ++i) e = std::max(e, std::fabs(double(yL[i]) - g * (a[i] + 0.5 * a[i])));
      CHECK(e < 1e-5, "dry -6 dB + wet -6 dB: err %g", e); }
}

static void testMeters() {
    std::printf("[chain] meters: IN after the input gain, OUT after the output gain\n");
    std::unique_ptr<ConvolutionProcessor> p(make()); p->loadEngine(delta(1.0f)); ProcessorParams q; q.inputGainDb = 6; q.outputGainDb = -3; p->setParams(q, true);
    V s = sine(96000, 1000.0, 48000.0, 0.1), yL, yR; runAll(*p, s, s, yL, yR, 64);
    const auto f = p->meterFrame();
    CHECK(std::fabs(f.peak[0] - (-20.0 + 6.0)) < 0.1, "IN peak after +6 dB input gain: %.2f (want -14.0)", f.peak[0]);
    CHECK(std::fabs(f.peak[2] - (-20.0 + 6.0 - 3.0)) < 0.15, "OUT peak after the output gain: %.2f (want -17.0)", f.peak[2]);
    CHECK(std::fabs(f.rms[1] - (-23.01 + 6.0)) < 0.1, "IN RMS %.2f", f.rms[1]);
    p->setMeterRmsMs(50); runAll(*p, s, s, yL, yR, 64); CHECK(std::fabs(p->meterFrame().rms[1] - (-23.01 + 6.0)) < 0.2, "50 ms window");
}

static void testPreDelay() {
    std::printf("[chain] pre-delay: exact value, click-free changes\n");
    { std::unique_ptr<ConvolutionProcessor> p(make()); p->loadEngine(delta(1.0f)); ProcessorParams q; q.preDelayMs = 10; p->setParams(q, true);
      V a(8000, 0.f), b(8000, 0.f), yL, yR; a[3000] = 1.0f;                                  // impulse after the engine fade
      V warm(2000, 0.f); V t1, t2; runAll(*p, warm, warm, t1, t2, 64);
      runAll(*p, a, b, yL, yR, 64);
      size_t peak = 0; for (size_t i = 0; i < yL.size(); ++i) if (std::fabs(yL[i]) > std::fabs(yL[peak])) peak = i;
      CHECK(peak == 3480 && std::fabs(yL[peak] - 1.0f) < 1e-6f, "impulse delayed to %zu (want 3480), value %g", peak, yL[peak]); }
    { std::unique_ptr<ConvolutionProcessor> p(make()); p->loadEngine(delta(1.0f)); ProcessorParams q; p->setParams(q, true);
      const V x = sine(6 * 16000 + 4000, 1000.0); V yL, yR, all;
      runAll(*p, V(x.begin(), x.begin() + 4000), V(4000, 0.f), yL, yR, 64);
      size_t pos = 4000; double worst = 0;
      for (int step = 0; step < 6; ++step) {                                               // jump the delay around while playing
          q.preDelayMs = std::vector<double>{0, 500, 3, 1200, 0.5, 0}[size_t(step)]; p->setParams(q);
          V a(x.begin() + long(pos), x.begin() + long(pos) + 16000), z(16000, 0.f); runAll(*p, a, z, yL, yR, 128); all.insert(all.end(), yL.begin(), yL.end()); pos += 16000; }
      for (size_t i = 1; i < all.size(); ++i) worst = std::max(worst, double(std::fabs(all[i] - all[i - 1])));
      const double limit = 0.5 * 2 * M_PI * 1000.0 / 48000.0 * 1.1;
      CHECK(worst < limit, "pre-delay jumps click: max step %g > %g", worst, limit); }
}

static void testWidthEqAndChain() {
    std::printf("[chain] width, EQ in the chain, block-size independence\n");
    V L = sine(40000, 700, 48000, 0.4), R = noise(40000, 9), yL, yR;
    { std::unique_ptr<ConvolutionProcessor> p(make()); p->loadEngine(delta(1.0f)); ProcessorParams q; q.width = 0; p->setParams(q, true); runAll(*p, L, R, yL, yR, 64);
      double e = 0; for (size_t i = 10000; i < L.size(); ++i) e = std::max(e, std::fabs(double(yL[i]) - 0.5 * (L[i] + R[i])));
      CHECK(e < 1e-5 && yL[20000] == yR[20000], "width 0 must give (L+R)/2 on both sides (err %g)", e);
      q.width = 2; p->setParams(q); runAll(*p, L, R, yL, yR, 64);
      e = 0; for (size_t i = 25000; i < L.size(); ++i) e = std::max(e, std::fabs(double(yL[i]) - (0.5 * (L[i] + R[i]) + (L[i] - R[i]))));
      CHECK(e < 1e-5, "width 2 must double the side signal (err %g)", e); }
    { std::unique_ptr<ConvolutionProcessor> p(make()); p->loadEngine(delta(1.0f)); ProcessorParams q; q.eq.hp = {true, 1000.0, 12}; p->setParams(q, true);
      V lo = sine(48000, 100.0), hi = sine(48000, 5000.0), z(48000, 0.f);
      runAll(*p, lo, z, yL, yR, 64); const double gLo = 20 * std::log10(rms(yL, 24000, 48000) / rms(lo, 24000, 48000));
      runAll(*p, hi, z, yL, yR, 64); const double gHi = 20 * std::log10(rms(yL, 24000, 48000) / rms(hi, 24000, 48000));
      CHECK(gLo < -18.0 && std::fabs(gHi) < 0.5, "HP 1 kHz in the chain: 100 Hz %.1f dB, 5 kHz %.2f dB", gLo, gHi); }
    { V a = noise(60000, 5), b = noise(60000, 6), y1, y2, y3, y4;
      IRBuffer ir; std::mt19937 rng(8); std::uniform_real_distribution<float> d(-1, 1); ir.ch.assign(2, V(3000)); for (auto& c : ir.ch) for (size_t i = 0; i < c.size(); ++i) c[i] = d(rng) * 0.05f * std::exp(-float(i) / 800.0f);
      ProcessorParams q; q.dryDb = -10; q.preDelayMs = 5; q.width = 1.4; q.eq.band[0] = {true, BandType::Bell, 800, 6, 1.5}; q.eq.lp = {true, 8000, 24};
      auto run = [&](int block, V& yl) { std::unique_ptr<ConvolutionProcessor> p(make()); p->loadEngine(StereoEngine::build(ir, InputMode::Stereo)); p->setParams(q, true);
          V warm(4000, 0.f), t1, t2; runAll(*p, warm, warm, t1, t2, 256); V yr; runAll(*p, a, b, yl, yr, block); };
      run(1, y1); run(64, y2); run(777, y3);
      double e12 = 0, e13 = 0; for (size_t i = 0; i < a.size(); ++i) { e12 = std::max(e12, double(std::fabs(y1[i] - y2[i]))); e13 = std::max(e13, double(std::fabs(y1[i] - y3[i]))); }
      CHECK(e12 < 2e-5 && e13 < 2e-5, "block-size independence: %g %g", e12, e13); }
}

static void testStress() {
    std::printf("[chain] stress: parameter changes + IR swaps + extreme input while running\n");
    ConvolutionProcessor p; p.prepare(44100.0);
    std::atomic<bool> stop{false};
    std::thread loader([&] {
        std::mt19937 rng(21); std::uniform_real_distribution<float> d(-1, 1);
        while (!stop.load()) {
            IRBuffer ir; ir.ch.assign(size_t(1 + (rng() % 3 == 0 ? 3 : rng() % 2)), V(1 + rng() % 30000)); for (auto& c : ir.ch) for (auto& v : c) v = d(rng) * 0.05f;
            p.loadEngine(StereoEngine::build(ir, InputMode(rng() % 4))); p.collectRetired();
            if (rng() % 10 == 0) p.unloadIR();
            std::this_thread::sleep_for(std::chrono::milliseconds(1 + rng() % 4));
        }
    });
    std::mt19937 rng(5); std::uniform_real_distribution<float> d(-1, 1);
    V x(3000), y(3000), z(3000); bool finite = true; double peak = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) {
        ProcessorParams q; q.inputGainDb = -20 + double(rng() % 40); q.dryDb = double(rng() % 60) - 50; q.wetDb = double(rng() % 20) - 10; q.preDelayMs = double(rng() % 300); q.width = double(rng() % 200) / 100.0;
        q.eq.band[0] = {true, BandType(rng() % 3), 50.0 + double(rng() % 10000), double(rng() % 36) - 18, 0.2 + double(rng() % 90) / 10.0};
        q.eq.hp = {rng() % 2 == 0, 20.0 + double(rng() % 900), int(12 << (rng() % 3))};
        p.setParams(q);
        for (auto& v : x) v = d(rng);
        if (rng() % 20 == 0) { x[100] = NAN; x[200] = INFINITY; }
        p.process(x.data(), x.data(), y.data(), z.data(), 1 + int(rng() % 3000));
        for (size_t i = 0; i < y.size(); ++i) { if (!std::isfinite(y[i]) || !std::isfinite(z[i])) finite = false; peak = std::max(peak, double(std::max(std::fabs(y[i]), std::fabs(z[i])))); }
    }
    stop = true; loader.join();
    CHECK(finite, "non-finite output"); std::printf("  peak output %.1f (finite: %d)\n", peak, int(finite));
}

static void benchChain() {
    std::printf("[chain] CPU, 48 kHz, 64-sample blocks, everything on (EQ 3 bands + HP/LP 24 dB, width, pre-delay)\n");
    std::mt19937 rng(2); std::uniform_real_distribution<float> d(-1, 1);
    for (size_t len : {2048u, 48000u}) for (int nch : {1, 2, 4}) {
        IRBuffer ir; ir.ch.assign(size_t(nch), V(len)); for (auto& c : ir.ch) for (auto& v : c) v = d(rng) * 0.02f;
        ConvolutionProcessor p; p.prepare(48000.0);
        ProcessorParams q; q.dryDb = -12; q.preDelayMs = 3; q.width = 1.3; q.eq.band[0] = {true, BandType::Bell, 400, 4, 1}; q.eq.band[1] = {true, BandType::Bell, 2500, -3, 2}; q.eq.band[2] = {true, BandType::HighShelf, 6000, 2, 0.7}; q.eq.hp = {true, 80, 24}; q.eq.lp = {true, 9000, 24};
        p.setParams(q, true); p.loadEngine(StereoEngine::build(ir, InputMode::Stereo));
        V L(64), R(64), yL(64), yR(64); for (auto& v : L) v = d(rng); for (auto& v : R) v = d(rng);
        for (int i = 0; i < 400; ++i) p.process(L.data(), R.data(), yL.data(), yR.data(), 64);
        auto now = [] { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
        double best = 1e12; for (int rep = 0; rep < 5; ++rep) { const double t0 = now(); for (int i = 0; i < 10000; ++i) p.process(L.data(), R.data(), yL.data(), yR.data(), 64); best = std::min(best, (now() - t0) / 10000.0); }
        std::printf("  IR %5zu taps, %d IR channel(s): %.2f us/call = %.2f%% of one core\n", len, nch, best, 100.0 * best / (1e6 * 64 / 48000.0));
    }
}

int main() {
    std::printf("FFT backend: %s\n", Convolver::fftName());
    testBasics(); testMeters(); testPreDelay(); testWidthEqAndChain(); testStress();
#ifndef LLC_TSAN_LITE
    benchChain();
#endif
    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL CHAIN TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
