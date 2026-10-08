#include "ir_display.hpp"
#include <chrono>
#include <cstdio>
using namespace llc;
static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
typedef std::vector<float> V;
static IRBuffer buf(std::vector<V> ch, double sr = 48000) { IRBuffer b; b.sampleRate = sr; b.ch = std::move(ch); return b; }
static float at(const IRDisplay& d, int t, double f) { const double x = std::log(f / d.fMin) / std::log(d.fMax / d.fMin) * double(d.spectrumDb[t].size() - 1); return d.spectrumDb[t][size_t(std::lround(x))]; }

int main() {
    std::printf("[display] wave and spectrum data for the UI\n");
    { V h(4800, 0.f); h[0] = 1.0f; auto d = computeIRDisplay(buf({h}), InputMode::Sum);
      CHECK(d.valid && d.traces == 1 && d.waveMax[0].size() == 800 && d.spectrumDb[0].size() == 256, "shape");
      CHECK(d.waveMax[0][0] == 1.0f && d.waveMax[0][1] == 0.0f && d.waveMin[0][0] == 0.0f, "delta wave columns");
      CHECK(std::fabs(d.durationMs - 100.0) < 1e-9 && d.lengthSamples == 4800 && d.peak == 1.0f, "duration %g", d.durationMs);
      double worst = 0; for (float v : d.spectrumDb[0]) worst = std::max(worst, std::fabs(double(v))); CHECK(worst < 0.1, "delta spectrum must be flat 0 dB (worst %g)", worst);
      CHECK(d.waveMax[1] == d.waveMax[0], "mono IR: both traces identical"); }
    { V h = {0.5f, 0.5f}; auto d = computeIRDisplay(buf({h}), InputMode::Sum);          // |H| = |cos(pi f / fs)|
      for (double f : {20.0, 1000.0, 5000.0, 12000.0, 20000.0}) { const double want = 20 * std::log10(std::fabs(std::cos(M_PI * f / 48000.0))); CHECK(std::fabs(at(d, 0, f) - want) < 0.35, "FIR lowpass @%g Hz: want %.2f got %.2f", f, want, at(d, 0, f)); } }
    { V a(3000, 0.f), b(3000, 0.f), c(3000, 0.f), e(3000, 0.f); a[10] = 1; b[20] = 0.5f; c[30] = 0.25f; e[40] = 0.125f;   // 4-channel IR: where is each impulse drawn ?
      auto col = [&](const IRDisplay& d, int t) { for (size_t i = 0; i < d.waveMax[t].size(); ++i) if (d.waveMax[t][i] != 0.f) return d.waveMax[t][i]; return 0.f; };
      IRBuffer ir = buf({a, b, c, e});
      auto s = computeIRDisplay(ir, InputMode::Stereo), m = computeIRDisplay(ir, InputMode::Sum), l = computeIRDisplay(ir, InputMode::Left), r = computeIRDisplay(ir, InputMode::Right);
      CHECK(s.traces == 2 && col(s, 0) == 1.0f && col(s, 1) == 0.125f, "true stereo / Stereo input: L = LL, R = RR");
      CHECK(l.waveMax[0].size() == 800 && col(l, 0) == 1.0f && col(l, 1) == 0.5f, "Left input: L = h0, R = h1");
      CHECK(col(r, 0) == 0.25f && col(r, 1) == 0.125f, "Right input: L = h2, R = h3");
      float m0 = 0, m1 = 0; for (float v : m.waveMax[0]) m0 = std::max(m0, v); for (float v : m.waveMax[1]) m1 = std::max(m1, v);
      CHECK(m0 == 1.0f && m1 == 0.5f, "Sum input: L = h0+h2 (impulses at different times), R = h1+h3 (%g, %g)", m0, m1); }
    { auto d = computeIRDisplay(buf({V(10, 0.3f)}), InputMode::Sum, 800, 256); CHECK(d.valid && d.waveMax[0].size() == 800, "10-sample IR on 800 columns"); }
    { auto d = computeIRDisplay(IRBuffer{}, InputMode::Sum); CHECK(!d.valid, "empty IR"); }
    { IRDisplayWorker w; uint32_t seen = 0; IRDisplay d;
      V h(2000, 0.f); h[5] = 1.0f;
      w.request([h](IRBuffer& ir, InputMode& m) { std::this_thread::sleep_for(std::chrono::milliseconds(150)); ir.sampleRate = 48000; ir.ch = {V(10, 0.1f)}; m = InputMode::Sum; return true; });
      w.request([h](IRBuffer& ir, InputMode& m) { ir.sampleRate = 48000; ir.ch = {h}; m = InputMode::Sum; return true; });          // replaces the first one
      bool got = false; for (int i = 0; i < 200 && !got; ++i) { got = w.fetch(d, seen); if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
      CHECK(got && d.valid && d.lengthSamples == 2000 && d.version == 1, "worker result: got=%d len=%zu version=%u", int(got), d.lengthSamples, d.version);
      CHECK(!w.fetch(d, d.version), "no new data -> fetch() must return false");
      w.request([](IRBuffer&, InputMode&) { return false; }); std::this_thread::sleep_for(std::chrono::milliseconds(100));
      IRDisplay e; CHECK(w.fetch(e, d.version) && !e.valid, "failed job -> invalid display with a new version"); }
    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL DISPLAY TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
