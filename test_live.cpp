#include "live_convolver.hpp"
#include "denormals.hpp"
#include <thread>
#include <chrono>
#include <cstdio>
#include <random>
using namespace llc;
static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::unique_ptr<StereoEngine> makeEngine(const std::vector<float>& h) {
    IRBuffer ir; ir.ch = {h};
    return StereoEngine::build(ir, InputMode::Stereo);
}
static std::unique_ptr<StereoEngine> makeDelta(float gain, size_t len = 1) {
    std::vector<float> h(len, 0.f); h[0] = gain; return makeEngine(h);
}
// Mono-style helper: the same signal on both channels; returns the left output.
static void run(LiveConvolver& lc, const float* in, float* out, int n) {
    std::vector<float> r(static_cast<size_t>(n), 0.f);
    lc.process(in, in, out, r.data(), n);
}

static void testCrossfade() {
    std::printf("[live] crossfade is linear, monotonic, click-free; passthrough before first IR\n");
    LiveConvolver lc(1024);
    std::vector<float> in(8192, 1.0f), out(8192, 0.f);
    run(lc, in.data(), out.data(), 512);
    CHECK(out[10] == 1.0f, "dry passthrough expected, got %g", out[10]);
    lc.load(makeDelta(0.5f));
    std::vector<float> y;
    for (int i = 0; i < 20; ++i) { run(lc, in.data(), out.data(), 300); y.insert(y.end(), out.begin(), out.begin() + 300); }
    CHECK(std::fabs(y.back() - 0.5f) < 1e-6f, "settled value %g", y.back());
    double maxStep = 0; for (size_t i = 1; i < y.size(); ++i) maxStep = std::max(maxStep, double(std::fabs(y[i] - y[i - 1])));
    CHECK(maxStep < 0.5 / 1024.0 * 1.05, "dry->wet step too large: %g", maxStep);
    lc.load(makeDelta(2.0f));
    y.clear();
    for (int i = 0; i < 20; ++i) { run(lc, in.data(), out.data(), 300); y.insert(y.end(), out.begin(), out.begin() + 300); }
    maxStep = 0; bool mono = true;
    for (size_t i = 1; i < y.size(); ++i) { maxStep = std::max(maxStep, double(std::fabs(y[i] - y[i - 1]))); if (y[i] < y[i - 1] - 1e-6f) mono = false; }
    CHECK(mono, "fade not monotonic");
    CHECK(maxStep < 1.5 / 1024.0 * 1.05, "swap step too large: %g", maxStep);
    CHECK(std::fabs(y.back() - 2.0f) < 1e-6f, "final %g", y.back());
    lc.collectRetired();
    lc.unload();
    for (int i = 0; i < 10; ++i) run(lc, in.data(), out.data(), 300);
    CHECK(std::fabs(out[299] - 1.0f) < 1e-6f, "after unload expected dry, got %g", out[299]);
}

static void testSwapExactness() {
    std::printf("[live] after the fade + IR length, output equals pure convolution with the new IR\n");
    std::mt19937 rng(11); std::uniform_real_distribution<float> d(-1, 1);
    const size_t N = 30000, LB = 3000;
    std::vector<float> x(N), hA(2000), hB(LB); for (auto& v : x) v = d(rng); for (auto& v : hA) v = d(rng) * 0.1f; for (auto& v : hB) v = d(rng) * 0.1f;
    LiveConvolver lc(512);
    lc.load(makeEngine(hA));
    std::vector<float> y(N);
    size_t pos = 0; bool swapped = false;
    while (pos < N) {
        int n = int(std::min<size_t>(1 + rng() % 300, N - pos));
        if (!swapped && pos > 8000) { lc.load(makeEngine(hB)); swapped = true; }
        run(lc, x.data() + pos, y.data() + pos, n); pos += size_t(n);
    }
    double err = 0;
    for (size_t n = 8000 + 512 + LB + 400; n < N; ++n) {
        double s = 0; for (size_t k = 0; k < LB && k <= n; ++k) s += double(hB[k]) * x[n - k];
        err = std::max(err, std::fabs(s - y[n]));
    }
    CHECK(err < 1e-4, "post-swap error %g", err);
}

static void testThreads() {
    std::printf("[live] stress: loader thread swaps IRs continuously while the audio thread runs\n");
    LiveConvolver lc(256);
    std::atomic<bool> stop{false};
    std::thread loader([&] {
        std::mt19937 rng(21); std::uniform_real_distribution<float> d(-1, 1);
        while (!stop.load()) {
            const size_t L = 1 + rng() % 20000;
            std::vector<float> h(L); for (auto& v : h) v = d(rng) * 0.05f;
            lc.load(makeEngine(h)); lc.collectRetired();
            if (rng() % 8 == 0) lc.unload();
            std::this_thread::sleep_for(std::chrono::milliseconds(1 + rng() % 4));
        }
    });
    std::mt19937 rng(5); std::uniform_real_distribution<float> d(-1, 1);
    std::vector<float> x(2048), y(2048); bool finite = true;
    const auto t0 = std::chrono::steady_clock::now();
    ScopedFlushDenormals ftz;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) {
        for (auto& v : x) v = d(rng);
        run(lc, x.data(), y.data(), 1 + int(rng() % 2048));
        for (float v : y) if (!std::isfinite(v)) finite = false;
    }
    stop = true; loader.join();
    CHECK(finite, "non-finite output during stress");
}

int main() {
    testCrossfade(); testSwapExactness(); testThreads();
    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL LIVE TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
