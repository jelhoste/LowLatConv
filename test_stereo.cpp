#include "stereo_engine.hpp"
#include <chrono>
#include <cstdio>
#include <random>
using namespace llc;
static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

typedef std::vector<float> V;
static V conv(const V& x, const V& h) {
    V y(x.size());
    for (size_t n = 0; n < x.size(); ++n) { double s = 0; for (size_t k = 0; k < h.size() && k <= n; ++k) s += double(h[k]) * x[n - k]; y[n] = float(s); }
    return y;
}
static void addTo(V& a, const V& b) { for (size_t i = 0; i < a.size(); ++i) a[i] += b[i]; }
static V scaleSum(const V& a, const V& b) { V r(a.size()); for (size_t i = 0; i < a.size(); ++i) r[i] = 0.5f * (a[i] + b[i]); return r; }
static double maxDiff(const V& a, const V& b) { double m = 0; for (size_t i = 0; i < a.size(); ++i) m = std::max(m, double(std::fabs(a[i] - b[i]))); return m; }

static void runEngine(StereoEngine& e, const V& L, const V& R, V& yL, V& yR, int mode, std::mt19937& rng, bool inplace) {
    yL.assign(L.size(), 0.f); yR.assign(L.size(), 0.f);
    size_t pos = 0;
    while (pos < L.size()) {
        int n = mode > 0 ? mode : 1 + int(rng() % 1500);
        n = int(std::min<size_t>(size_t(n), L.size() - pos));
        if (inplace) { V a(L.begin() + long(pos), L.begin() + long(pos) + n), b(R.begin() + long(pos), R.begin() + long(pos) + n); e.process(a.data(), b.data(), a.data(), b.data(), n); std::copy(a.begin(), a.end(), yL.begin() + long(pos)); std::copy(b.begin(), b.end(), yR.begin() + long(pos)); }
        else e.process(L.data() + pos, R.data() + pos, yL.data() + pos, yR.data() + pos, n);
        pos += size_t(n);
    }
}

static void testRouting() {
    std::printf("[stereo] every IR layout x input mode x block size vs direct convolution\n");
    std::mt19937 rng(12); std::uniform_real_distribution<float> d(-1, 1);
    const size_t N = 9000; V L(N), R(N); for (auto& v : L) v = d(rng); for (auto& v : R) v = d(rng);
    int cases = 0;
    for (size_t len : {1u, 63u, 64u, 200u, 3000u})
        for (size_t nc : {1u, 2u, 3u, 4u, 5u}) {
            IRBuffer ir; ir.ch.assign(nc, V(len)); for (auto& c : ir.ch) for (size_t i = 0; i < len; ++i) c[i] = d(rng) * 0.3f * std::exp(-3.0f * float(i) / float(len));
            const size_t u = nc == 3 ? 2 : std::min<size_t>(nc, 4);
            const V& h0 = ir.ch[0]; const V& h1 = ir.ch[u > 1 ? 1 : 0]; const V h2 = u == 4 ? ir.ch[2] : h0; const V h3 = u == 4 ? ir.ch[3] : h0;
            for (InputMode mode : {InputMode::Stereo, InputMode::Sum, InputMode::Left, InputMode::Right}) {
                V m = mode == InputMode::Sum ? scaleSum(L, R) : (mode == InputMode::Left ? L : R);
                V eL, eR;
                if (mode == InputMode::Stereo) {
                    if (u == 1) { eL = conv(L, h0); eR = conv(R, h0); }
                    else if (u == 2) { eL = conv(L, h0); eR = conv(R, h1); }
                    else { eL = conv(L, h0); addTo(eL, conv(R, h2)); eR = conv(L, h1); addTo(eR, conv(R, h3)); }
                } else if (u == 1) { eL = eR = conv(m, h0); }
                else if (u == 2) { eL = conv(m, h0); eR = conv(m, h1); }
                else if (mode == InputMode::Sum) { eL = conv(m, h0); addTo(eL, conv(m, h2)); eR = conv(m, h1); addTo(eR, conv(m, h3)); }
                else if (mode == InputMode::Left) { eL = conv(m, h0); eR = conv(m, h1); }
                else { eL = conv(m, h2); eR = conv(m, h3); }
                for (int blk : {1, 33, 64, 0}) for (bool inplace : {false, true}) {
                    ConvolverConfig cfg; cfg.headSize = 32; cfg.maxPartition = 256;
                    auto e = StereoEngine::build(ir, mode, cfg);
                    CHECK(e != nullptr, "build len=%zu nc=%zu", len, nc);
                    if (!e) continue;
                    V yL, yR; runEngine(*e, L, R, yL, yR, blk, rng, inplace);
                    const double err = std::max(maxDiff(yL, eL), maxDiff(yR, eR));
                    ++cases;
                    CHECK(err < 3e-5, "len=%zu irCh=%zu mode=%d block=%d inplace=%d err=%g", len, nc, int(mode), blk, int(inplace), err);
                }
            }
        }
    std::printf("  %d configurations checked\n", cases);
}

static void testMatrix() {
    std::printf("[stereo] random path matrices with different lengths per path\n");
    std::mt19937 rng(31); std::uniform_real_distribution<float> d(-1, 1);
    const size_t N = 12000; int cases = 0;
    for (int it = 0; it < 60; ++it) {
        const int nIn = 1 + int(rng() % 3), nOut = 1 + int(rng() % 3), nPaths = 1 + int(rng() % 5);
        std::vector<V> irs; std::vector<PathSpec> ps;
        static const size_t lens[] = {1, 40, 64, 70, 300, 1500, 6000};
        for (int k = 0; k < nPaths; ++k) { V h(lens[rng() % 7]); for (auto& v : h) v = d(rng) * 0.2f; irs.push_back(h); }
        for (int k = 0; k < nPaths; ++k) { PathSpec p; p.in = int(rng() % size_t(nIn)); p.out = int(rng() % size_t(nOut)); p.ir = irs[size_t(k)].data(); p.length = irs[size_t(k)].size(); ps.push_back(p); }
        std::vector<V> x(static_cast<size_t>(nIn), V(N)); for (auto& c : x) for (auto& v : c) v = d(rng);
        std::vector<V> ref(static_cast<size_t>(nOut), V(N, 0.f));
        for (int k = 0; k < nPaths; ++k) addTo(ref[size_t(ps[size_t(k)].out)], conv(x[size_t(ps[size_t(k)].in)], irs[size_t(k)]));
        ConvolverConfig cfg; cfg.headSize = (it % 2) ? 32 : 64; cfg.maxPartition = (it % 3) ? 256 : 1024;
        Convolver c; CHECK(c.prepare(nIn, nOut, ps.data(), nPaths, cfg), "prepare");
        std::vector<V> y(static_cast<size_t>(nOut), V(N, 0.f));
        size_t pos = 0;
        while (pos < N) {
            const int n = int(std::min<size_t>(size_t(1 + rng() % 400), N - pos));
            const float* ins[4]; float* outs[4];
            for (int i = 0; i < nIn; ++i) ins[i] = x[size_t(i)].data() + pos;
            for (int o = 0; o < nOut; ++o) outs[o] = y[size_t(o)].data() + pos;
            c.process(ins, outs, n); pos += size_t(n);
        }
        double err = 0; for (int o = 0; o < nOut; ++o) err = std::max(err, maxDiff(y[size_t(o)], ref[size_t(o)]));
        ++cases; CHECK(err < 3e-5, "matrix %d (%dx%d, %d paths): err %g", it, nIn, nOut, nPaths, err);
    }
    std::printf("  %d random matrices checked\n", cases);
    Convolver bad; PathSpec p; CHECK(!bad.prepare(1, 1, &p, 1), "null IR must fail");
    float one = 1; p.ir = &one; p.length = 1; p.out = 3; CHECK(!bad.prepare(1, 1, &p, 1), "out-of-range output must fail");
}

static void benchShared() {
    std::printf("[stereo] cost of true stereo (4 paths x 2048 taps), 64-sample blocks, shared FFTs vs 4 independent mono engines\n");
    std::mt19937 rng(2); std::uniform_real_distribution<float> d(-1, 1);
    IRBuffer ir; ir.ch.assign(4, V(2048)); for (auto& c : ir.ch) for (auto& v : c) v = d(rng) * 0.05f;
    auto e = StereoEngine::build(ir, InputMode::Stereo);
    std::vector<Convolver> mono(4); for (int k = 0; k < 4; ++k) mono[size_t(k)].prepare(ir.ch[size_t(k)].data(), 2048);
    V L(64), R(64), yL(64), yR(64), t(64); for (auto& v : L) v = d(rng); for (auto& v : R) v = d(rng);
    auto now = [] { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    double best1 = 1e12, best2 = 1e12;
    for (int rep = 0; rep < 5; ++rep) {
        double t0 = now(); for (int i = 0; i < 20000; ++i) e->process(L.data(), R.data(), yL.data(), yR.data(), 64); best1 = std::min(best1, (now() - t0) / 20000);
        t0 = now(); for (int i = 0; i < 20000; ++i) for (int k = 0; k < 4; ++k) mono[size_t(k)].process(L.data(), t.data(), 64); best2 = std::min(best2, (now() - t0) / 20000);
    }
    std::printf("  shared: %.2f us/call   4 independent engines: %.2f us/call   gain x%.2f\n", best1, best2, best2 / best1);
}

int main() {
    std::printf("FFT backend: %s\n", Convolver::fftName());
    testRouting(); testMatrix(); benchShared();
    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL STEREO TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
