#include "eq.hpp"
#include <cstdio>
#include <random>
#include <vector>
using namespace llc;
static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static double gainDbAt(const EQParams& p, double f, double fs = 48000.0) {
    ParametricEQ eq; eq.prepare(fs); eq.setParams(p, true);
    const size_t N = size_t(fs * 1.5);
    std::vector<float> x(N), y;
    for (size_t n = 0; n < N; ++n) x[n] = float(0.5 * std::sin(2.0 * M_PI * f * double(n) / fs));
    y = x;
    for (size_t pos = 0; pos < N;) { int k = int(std::min<size_t>(64, N - pos)); eq.process(y.data() + pos, nullptr, k); pos += size_t(k); }
    const double cycles = std::floor(0.5 * f);
    const size_t W = size_t(std::round(std::max(1.0, cycles) * fs / f));
    double sx = 0, sy = 0;
    for (size_t n = N - W; n < N; ++n) { sx += double(x[n]) * x[n]; sy += double(y[n]) * y[n]; }
    return 10.0 * std::log10(sy / sx);
}

static void testBands() {
    std::printf("[eq] bell / shelf responses\n");
    EQParams p; p.band[0] = {true, BandType::Bell, 1000, 12, 1.0};
    CHECK(std::fabs(gainDbAt(p, 1000) - 12.0) < 0.05, "bell +12 @fc = %g", gainDbAt(p, 1000));
    CHECK(std::fabs(gainDbAt(p, 60)) < 0.5 && std::fabs(gainDbAt(p, 16000)) < 0.8, "bell far-field");
    p.band[0].gainDb = -12; CHECK(std::fabs(gainDbAt(p, 1000) + 12.0) < 0.05, "bell -12 @fc");
    p.band[0] = {true, BandType::LowShelf, 200, 9, 0.707};
    CHECK(std::fabs(gainDbAt(p, 20) - 9.0) < 0.25 && std::fabs(gainDbAt(p, 10000)) < 0.25, "low shelf asymptotes %g %g", gainDbAt(p, 20), gainDbAt(p, 10000));
    p.band[0] = {true, BandType::HighShelf, 4000, -8, 0.707};
    CHECK(std::fabs(gainDbAt(p, 16000) + 8.0) < 0.6 && std::fabs(gainDbAt(p, 100)) < 0.1, "high shelf asymptotes %g %g", gainDbAt(p, 16000), gainDbAt(p, 100));
    EQParams q; q.band[0] = {true, BandType::Bell, 500, 6, 2}; q.band[1] = {true, BandType::Bell, 500, 6, 2};
    CHECK(std::fabs(gainDbAt(q, 500) - 12.0) < 0.1, "two bells stack");
}

static double butter(double f, double fc, int n, bool hp, double fs) {      // exact bilinear Butterworth magnitude
    const double r = std::tan(M_PI * f / fs) / std::tan(M_PI * fc / fs);
    return -10.0 * std::log10(1.0 + std::pow(hp ? 1.0 / r : r, 2.0 * n));
}
static void testPass() {
    std::printf("[eq] high-pass / low-pass: corner and slopes (12/24/48 dB/oct)\n");
    for (int slope : {12, 24, 48}) {
        const int n = slope / 6;
        EQParams p; p.hp = {true, 1000, slope};
        for (double f : {250.0, 500.0, 1000.0, 2000.0, 8000.0}) {
            const double want = butter(f, 1000, n, true, 48000), got = gainDbAt(p, f);
            CHECK(std::fabs(want - got) < 0.15 + 0.002 * std::fabs(want), "HP %d dB/oct @%g Hz: want %.2f got %.2f", slope, f, want, got);
        }
        EQParams q; q.lp = {true, 2000, slope};
        for (double f : {500.0, 1000.0, 2000.0, 4000.0, 8000.0}) {
            const double want = butter(f, 2000, n, false, 48000), got = gainDbAt(q, f);
            CHECK(std::fabs(want - got) < 0.15 + 0.002 * std::fabs(want), "LP %d dB/oct @%g Hz: want %.2f got %.2f", slope, f, want, got);
        }
    }
    EQParams p; p.hp = {true, 1000, 24};
    std::printf("  HP 24 dB/oct, 1 kHz: -3 dB @fc = %.2f, one octave below = %.2f dB\n", gainDbAt(p, 1000), gainDbAt(p, 500));
}

static void testBypassAndFuzz() {
    std::printf("[eq] exact bypass, stability fuzz, click-free parameter changes\n");
    std::mt19937 rng(4); std::uniform_real_distribution<float> d(-1, 1);
    { ParametricEQ eq; eq.prepare(48000); eq.setParams(EQParams{}, true);
      std::vector<float> a(5000), b; for (auto& v : a) v = d(rng); b = a; eq.process(b.data(), nullptr, 5000);
      CHECK(a == b, "default (all disabled) must be a bit-exact bypass"); }
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        ParametricEQ eq; eq.prepare(fs); EQParams p; eq.setParams(p, true);
        std::vector<float> l(4096), r(4096); bool ok = true; double peak = 0;
        for (int it = 0; it < 400; ++it) {
            for (auto& b : p.band) { b.enabled = rng() % 4 != 0; b.type = BandType(rng() % 3); b.freq = 20.0 * std::pow(1000.0, double(rng() % 1000) / 999.0); b.gainDb = -18.0 + 36.0 * double(rng() % 1000) / 999.0; b.q = 0.1 * std::pow(100.0, double(rng() % 1000) / 999.0); }
            p.hp = {rng() % 2 == 0, 20.0 + double(rng() % 980), int(12 << (rng() % 3))};
            p.lp = {rng() % 2 == 0, 1000.0 + double(rng() % 19000), int(12 << (rng() % 3))};
            eq.setParams(p);
            for (size_t i = 0; i < l.size(); ++i) { l[i] = d(rng); r[i] = d(rng); }
            eq.process(l.data(), r.data(), int(1 + rng() % 4096));
            for (size_t i = 0; i < l.size(); ++i) { if (!std::isfinite(l[i]) || !std::isfinite(r[i])) ok = false; peak = std::max(peak, double(std::max(std::fabs(l[i]), std::fabs(r[i])))); }
        }
        CHECK(ok && peak < 1e4, "fuzz fs=%g finite=%d peak=%g", fs, int(ok), peak);
    }
    // Off-centre, high-Q gain change: the phase response changes too, so a hard switch clicks.
    auto maxStep = [&](bool snap, double& steadyStep) {
        auto tone = [](size_t n) { return float(0.5 * std::sin(2.0 * M_PI * 1200.0 * double(n) / 48000.0)); };
        ParametricEQ eq; eq.prepare(48000); EQParams p; p.band[0] = {true, BandType::Bell, 1000, 0, 8}; eq.setParams(p, true);
        std::vector<float> y(48000); for (size_t n = 0; n < y.size(); ++n) y[n] = tone(n);
        eq.process(y.data(), nullptr, 9600);
        p.band[0].gainDb = 18; eq.setParams(p, snap);
        eq.process(y.data() + 9600, nullptr, int(y.size()) - 9600);
        double m = 0; for (size_t n = 1; n < y.size(); ++n) m = std::max(m, double(std::fabs(y[n] - y[n - 1])));
        ParametricEQ e2; e2.prepare(48000); e2.setParams(p, true);
        std::vector<float> z(48000); for (size_t n = 0; n < z.size(); ++n) z[n] = tone(n);
        e2.process(z.data(), nullptr, int(z.size()));
        steadyStep = 0; for (size_t n = 40000; n < z.size(); ++n) steadyStep = std::max(steadyStep, double(std::fabs(z[n] - z[n - 1])));
        return m;
    };
    double steady = 0, dummy = 0;
    const double smooth = maxStep(false, steady), hard = maxStep(true, dummy);
    std::printf("  max sample step: smoothed %.3f, hard switch %.3f, steady-state limit %.3f\n", smooth, hard, steady);
    CHECK(smooth < steady * 1.03, "smoothed gain change clicks: %g > %g", smooth, steady * 1.03);
    CHECK(hard > steady * 1.2, "test is not sensitive (hard switch %g vs steady %g)", hard, steady);
}

int main() {
    testBands(); testPass(); testBypassAndFuzz();
    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL EQ TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
