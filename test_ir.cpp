#include "ir_processor.hpp"
#include "convolver.hpp"
#include <cstdio>
#include <random>
using namespace llc;
static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static IROptions plain() { IROptions o; o.autoTrimStart = false; o.norm = NormMode::Off; return o; }
static IRBuffer mono(const std::vector<float>& v, double sr = 48000) { IRBuffer b; b.sampleRate = sr; b.ch = {v}; return b; }

// least-squares fit of a*sin + b*cos at frequency f over [from,to): returns max residual
static double sineResidual(const std::vector<float>& y, double f, double fs, size_t from, size_t to, double* amp = nullptr) {
    double ss = 0, cc = 0, sc = 0, sy = 0, cy = 0;
    for (size_t n = from; n < to; ++n) { const double s = std::sin(2 * M_PI * f * double(n) / fs), c = std::cos(2 * M_PI * f * double(n) / fs); ss += s * s; cc += c * c; sc += s * c; sy += s * y[n]; cy += c * y[n]; }
    const double det = ss * cc - sc * sc, a = (sy * cc - cy * sc) / det, b = (cy * ss - sy * sc) / det;
    double r = 0; for (size_t n = from; n < to; ++n) r = std::max(r, std::fabs(y[n] - (a * std::sin(2 * M_PI * f * double(n) / fs) + b * std::cos(2 * M_PI * f * double(n) / fs))));
    if (amp) *amp = std::sqrt(a * a + b * b);
    return r;
}

static void testResample() {
    std::printf("[ir] sample-rate conversion and stretch\n");
    struct C { double src, dst; } cs[] = {{48000, 96000}, {96000, 48000}, {44100, 48000}, {48000, 44100}, {48000, 24000}};
    for (auto c : cs) {
        std::vector<float> x(size_t(c.src)); for (size_t n = 0; n < x.size(); ++n) x[n] = float(0.7 * std::sin(2 * M_PI * 1000.0 * double(n) / c.src));
        IROptions o = plain(); o.targetSampleRate = c.dst; IRBuffer out;
        CHECK(processIR(mono(x, c.src), o, out), "processIR failed");
        const size_t L = out.length(); double amp = 0;
        const double r = sineResidual(out.ch[0], 1000.0, c.dst, 3000, L - 3000, &amp);
        CHECK(std::fabs(double(L) - c.dst) <= 1.0, "length %zu vs %g", L, c.dst);
        CHECK(r < 1e-3 && std::fabs(amp - 0.7) < 1e-3, "%g->%g: residual %g amp %g", c.src, c.dst, r, amp);
    }
    { // anti-aliasing: 15 kHz tone must vanish when downsampling to 24 kHz
        std::vector<float> x(48000); for (size_t n = 0; n < x.size(); ++n) x[n] = float(std::sin(2 * M_PI * 15000.0 * double(n) / 48000.0));
        IROptions o = plain(); o.targetSampleRate = 24000; IRBuffer out; processIR(mono(x), o, out);
        double e = 0; for (size_t n = 3000; n < out.length() - 3000; ++n) e = std::max(e, double(std::fabs(out.ch[0][n])));
        CHECK(e < 1e-3, "alias level %g (-60 dB = 1e-3)", e);
    }
    { // stretch 2: twice as long, one octave lower
        std::vector<float> x(48000); for (size_t n = 0; n < x.size(); ++n) x[n] = float(0.5 * std::sin(2 * M_PI * 1000.0 * double(n) / 48000.0));
        IROptions o = plain(); o.stretch = 2.0; IRBuffer out; processIR(mono(x), o, out);
        CHECK(out.length() == 96000, "stretched length %zu", out.length());
        CHECK(sineResidual(out.ch[0], 500.0, 48000, 3000, out.length() - 3000) < 1e-3, "stretched tone is not 500 Hz");
    }
}

static void testEdits() {
    std::printf("[ir] trims, length, envelope, reverse, normalisation, robustness\n");
    std::mt19937 rng(2); std::uniform_real_distribution<float> d(-1, 1);
    std::vector<float> h(20000); for (size_t i = 0; i < h.size(); ++i) h[i] = d(rng) * std::exp(-float(i) / 2000.0f);
    { std::vector<float> z(100, 0.f); z.insert(z.end(), h.begin(), h.end());
      IROptions o = plain(); o.autoTrimStart = true; IRBuffer out; processIR(mono(z), o, out);
      CHECK(out.length() == h.size() && out.ch[0][0] == h[0], "auto trim: len %zu (want %zu)", out.length(), h.size()); }
    { IROptions o = plain(); o.autoLength = true; o.lengthDb = -60; IRBuffer out; processIR(mono(h), o, out);
      CHECK(out.length() > 9000 && out.length() < 20000, "auto length -> %zu", out.length()); }
    { IROptions o = plain(); o.beginMs = 10; o.endMs = 100; IRBuffer out; processIR(mono(h), o, out);
      CHECK(out.length() == size_t(90 * 48), "begin 10 ms / end 100 ms -> %zu samples (want 4320)", out.length());
      CHECK(std::fabs(out.ch[0][0]) < 1e-6f && std::fabs(out.ch[0].back()) < 1e-3f, "edge fades missing"); }
    { std::vector<float> z(100, 0.f); z.insert(z.end(), h.begin(), h.end());           // Begin / End are measured from the auto-trimmed start
      IROptions o = plain(); o.autoTrimStart = true; o.endMs = 50; IRBuffer out; processIR(mono(z), o, out);
      CHECK(out.length() == size_t(50 * 48), "auto trim + end 50 ms -> %zu samples (want 2400)", out.length());
      o.beginMs = 10; processIR(mono(z), o, out); CHECK(out.length() == size_t(40 * 48), "auto trim + begin 10 + end 50 -> %zu (want 1920)", out.length());
      o.autoLength = true; o.lengthDb = -20; processIR(mono(z), o, out); CHECK(out.length() <= size_t(40 * 48), "auto length can only shorten (%zu)", out.length()); }
    { IROptions o = plain(); o.reverse = true; IRBuffer out; processIR(mono(h), o, out);
      bool ok = out.length() == h.size(); for (size_t i = 0; ok && i < h.size(); ++i) ok = out.ch[0][i] == h[h.size() - 1 - i];
      CHECK(ok, "reverse"); }
    { IROptions o = plain(); o.norm = NormMode::Peak; IRBuffer out; processIR(mono(h), o, out);
      double p = 0; for (float v : out.ch[0]) p = std::max(p, double(std::fabs(v))); CHECK(std::fabs(p - 1.0) < 1e-6, "peak norm %g", p);
      o.norm = NormMode::Energy; processIR(mono(h), o, out);
      double e = 0; for (float v : out.ch[0]) e += double(v) * v; CHECK(std::fabs(e - 1.0) < 1e-5, "energy norm %g", e); }
    { std::vector<float> ones(1000, 1.0f); IROptions o = plain(); o.attackMs = 10; o.decayDb = 20; IRBuffer out; processIR(mono(ones), o, out);
      CHECK(out.ch[0][0] == 0.0f, "attack start %g", out.ch[0][0]);
      CHECK(std::fabs(out.ch[0][500] - std::pow(10.0f, -20.0f * 500.0f / 999.0f / 20.0f)) < 1e-5f, "attack must be finished at 500: %g", out.ch[0][500]);
      CHECK(std::fabs(out.ch[0].back() - 0.1f) < 1e-5f, "decay end %g", out.ch[0].back());
      IROptions o2 = plain(); o2.attackMs = 10; o2.attackShape = 1; IRBuffer a; processIR(mono(ones), o2, a);
      o2.attackShape = -1; IRBuffer b; processIR(mono(ones), o2, b);
      CHECK(a.ch[0][100] > 0.5f && b.ch[0][100] < 0.1f, "attack shape %g / %g", a.ch[0][100], b.ch[0][100]); }
    { std::vector<float> bad = h; bad[5] = NAN; bad[9] = INFINITY; IROptions o = plain(); IRBuffer out;
      CHECK(processIR(mono(bad), o, out), "NaN IR");
      bool fin = true; for (float v : out.ch[0]) if (!std::isfinite(v)) fin = false; CHECK(fin, "non-finite output");
      CHECK(!processIR(IRBuffer{}, o, out), "empty must fail");
      CHECK(!processIR(mono(std::vector<float>(100, 0.f)), plain(), out), "silent IR must fail");
      IROptions big = plain(); big.stretch = 100.0; processIR(mono(h), big, out); CHECK(out.length() <= h.size() * 4 + 1 && out.length() >= h.size() * 4 - 1, "stretch clamp %zu", out.length());
      IROptions cap = plain(); cap.maxLength = 5000; processIR(mono(h), cap, out); CHECK(out.length() == 5000, "max length cap %zu", out.length()); }
    { IRBuffer st; st.ch = {h, std::vector<float>(h.begin(), h.end() - 3)}; IROptions o = plain(); IRBuffer out;
      CHECK(processIR(st, o, out) && out.ch[0].size() == out.ch[1].size(), "channels padded to equal length"); }
}

static void testWithConvolver() {
    std::printf("[ir] prepared IR through the convolver\n");
    std::mt19937 rng(8); std::uniform_real_distribution<float> d(-1, 1);
    std::vector<float> h(30000); for (size_t i = 0; i < h.size(); ++i) h[i] = d(rng) * std::exp(-float(i) / 3000.0f);
    IROptions o; o.targetSampleRate = 44100; o.autoLength = true; IRBuffer out; processIR(mono(h, 48000), o, out);
    Convolver c; CHECK(c.prepare(out.ch[0].data(), out.length()), "prepare");
    std::vector<float> x(8000), y(8000); for (auto& v : x) v = d(rng);
    c.process(x.data(), y.data(), 8000);
    double err = 0; for (size_t n = 0; n < x.size(); ++n) { double s = 0; for (size_t k = 0; k <= n && k < out.length(); ++k) s += double(out.ch[0][k]) * x[n - k]; err = std::max(err, std::fabs(s - y[n])); }
    CHECK(err < 1e-4, "convolution of the prepared IR: err %g", err);
}

int main() {
    testResample(); testEdits(); testWithConvolver();
    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL IR TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
