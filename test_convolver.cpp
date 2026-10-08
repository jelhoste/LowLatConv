#include "convolver.hpp"
#include <chrono>
#include <cstdio>
#include <random>
#include <cstdlib>
#include <string>
using namespace llc;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

template <class FFT>
static void testBackend() {
    std::printf("[fft] backend '%s': circular convolution via forward/mulAcc/inverse vs naive, round trip\n", FFT::name());
    std::mt19937 rng(1); std::uniform_real_distribution<float> d(-1, 1);
    for (size_t n : {32, 64, 128, 1024, 4096}) {
        if (int(n) < FFT::minSize()) continue;
        FFT f; CHECK(f.init(n), "init %zu", n);
        const size_t sf = f.specFloats();
        AlignedArray<float> a(n), b(n), t(n), A(sf), B(sf), C(sf), r(n);
        for (size_t i = 0; i < n; ++i) { a[i] = d(rng); b[i] = d(rng); }
        f.forward(a.data(), A.data()); f.forward(b.data(), B.data());
        f.mulAcc(A.data(), B.data(), C.data());
        f.mulAcc(A.data(), B.data(), C.data());                   // accumulate twice -> 2 * (a (*) b)
        f.inverse(C.data(), r.data());
        double err = 0, mag = 0;
        for (size_t k = 0; k < n; ++k) {
            double s = 0; for (size_t j = 0; j < n; ++j) s += double(a[j]) * double(b[(k + n - j) % n]);
            err = std::max(err, std::fabs(2.0 * s - double(r[k]) * f.inverseScale()));
            mag = std::max(mag, std::fabs(2.0 * s));
        }
        CHECK(err < 3e-6 * std::max(1.0, mag) * std::sqrt(double(n)), "n=%zu conv err %g (mag %g)", n, err, mag);
        f.forward(a.data(), A.data()); f.inverse(A.data(), t.data());
        double rt = 0; for (size_t k = 0; k < n; ++k) rt = std::max(rt, double(std::fabs(t[k] * f.inverseScale() - a[k])));
        CHECK(rt < 1e-5, "n=%zu round trip %g", n, rt);
    }
}

static void testFFT() {
    testBackend<BuiltinSpectralFFT>();
#ifdef LLC_USE_PFFFT
    testBackend<PffftSpectralFFT>();
#endif
}

static std::vector<float> refConv(const std::vector<float>& x, const std::vector<float>& h) {
    std::vector<float> y(x.size());
    for (size_t n = 0; n < x.size(); ++n) {
        double s = 0;
        const size_t kmax = std::min(n + 1, h.size());
        for (size_t k = 0; k < kmax; ++k) s += double(h[k]) * double(x[n - k]);
        y[n] = float(s);
    }
    return y;
}

static void runBlocks(Convolver& c, const std::vector<float>& x, std::vector<float>& y, std::mt19937& rng, int mode, bool inplace) {
    y.assign(x.size(), 0.f);
    size_t pos = 0;
    while (pos < x.size()) {
        int n = mode > 0 ? mode : 1 + int(rng() % 777);
        n = int(std::min<size_t>(size_t(n), x.size() - pos));
        if (inplace) { std::vector<float> tmp(x.begin() + pos, x.begin() + pos + n); c.process(tmp.data(), tmp.data(), n); std::copy(tmp.begin(), tmp.end(), y.begin() + pos); }
        else c.process(x.data() + pos, y.data() + pos, n);
        pos += size_t(n);
    }
}

static void testConvolver() {
    std::printf("[conv] exactness vs reference, many IR lengths / head sizes / host block sizes\n");
    std::mt19937 rng(7); std::uniform_real_distribution<float> d(-1, 1);
    const size_t N = 40000;
    std::vector<float> x(N);
    for (auto& v : x) v = d(rng);
    int cases = 0;
    for (size_t L : {1u, 2u, 17u, 64u, 65u, 127u, 129u, 1000u, 4096u, 5000u, 20000u}) {
        std::vector<float> h(L);
        for (size_t i = 0; i < L; ++i) h[i] = d(rng) * std::exp(-4.0f * float(i) / float(L)); 
        const auto ref = refConv(x, h);
        double refMax = 0; for (float v : ref) refMax = std::max(refMax, double(std::fabs(v)));
        for (int head : {16, 64, 256})
            for (int maxP : {256, 4096}) {
                ConvolverConfig cfg; cfg.headSize = head; cfg.maxPartition = maxP;
                for (int mode : {1, 7, 64, 100, 513, 0}) {
                    Convolver c; CHECK(c.prepare(h.data(), L, cfg), "prepare");
                    std::vector<float> y; runBlocks(c, x, y, rng, mode, (mode & 1) != 0);
                    double err = 0; for (size_t i = 0; i < N; ++i) err = std::max(err, double(std::fabs(y[i] - ref[i])));
                    ++cases;
                    CHECK(err < 2e-5 * std::max(1.0, refMax), "L=%zu head=%d maxP=%d block=%d err=%g (refMax %g)", L, head, maxP, mode, err, refMax);
                }
            }
    }
    std::printf("  %d configurations checked\n", cases);
}

static void testZeroLatency() {
    std::printf("[conv] zero latency: first output sample == h[0]*x[0]\n");
    std::vector<float> h(3000, 0.001f); h[0] = 0.7f;
    Convolver c; c.prepare(h.data(), h.size());
    float in = 1.f, out = 0.f; c.process(&in, &out, 1);
    CHECK(std::fabs(out - 0.7f) < 1e-6f, "out=%g", out);
    CHECK(c.latencySamples() == 0, "latency");
}

static void testRobustness() {
    std::printf("[conv] robustness: NaN/Inf input, reset, empty IR, huge block\n");
    std::mt19937 rng(3); std::uniform_real_distribution<float> d(-1, 1);
    std::vector<float> h(5000); for (auto& v : h) v = d(rng) * 0.1f;
    Convolver c; c.prepare(h.data(), h.size());
    std::vector<float> x(20000), y(20000);
    for (auto& v : x) v = d(rng);
    x[100] = NAN; x[5000] = INFINITY; x[9000] = -INFINITY;
    c.process(x.data(), y.data(), int(x.size()));
    bool finite = true; for (float v : y) if (!std::isfinite(v)) finite = false;
    CHECK(finite, "output contains non-finite values after NaN/Inf input");
    c.reset();
    std::vector<float> z(2000, 0.f), o(2000, 1.f);
    c.process(z.data(), o.data(), 2000);
    double e = 0; for (float v : o) e = std::max(e, double(std::fabs(v)));
    CHECK(e == 0.0, "reset did not clear state (%g)", e);
    Convolver empty; CHECK(!empty.prepare(nullptr, 0), "empty prepare should fail");
    std::vector<float> a(10, 1.f), b(10, 1.f); empty.process(a.data(), b.data(), 10);
    CHECK(b[0] == 0.f, "unprepared convolver must output silence");
    // block much larger than any partition
    std::vector<float> big(100000), bigOut(100000), bigRef;
    for (auto& v : big) v = d(rng);
    Convolver c2; c2.prepare(h.data(), h.size()); c2.process(big.data(), bigOut.data(), 100000);
    bigRef = refConv(big, h);
    double err = 0; for (size_t i = 0; i < big.size(); ++i) err = std::max(err, double(std::fabs(bigOut[i] - bigRef[i])));
    CHECK(err < 1e-4, "huge block err=%g", err);
}

int main() {
    std::printf("convolver FFT backend: %s\n", Convolver::fftName());
    testFFT(); testZeroLatency(); testConvolver(); testRobustness();
    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
