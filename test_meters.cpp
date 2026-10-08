#include "meters.hpp"
#include <cstdio>
#include <random>
using namespace llc;
static constexpr int InL = LevelMeters::InL, InR = LevelMeters::InR, OutL = LevelMeters::OutL;
static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
typedef std::vector<float> V;
static V sine(size_t n, double f, double amp, double fs) { V v(n); for (size_t i = 0; i < n; ++i) v[i] = float(amp * std::sin(2 * M_PI * f * double(i) / fs)); return v; }
static void feed(LevelMeters& m, const V& l, const V& r, int block, bool out = false) { for (size_t p = 0; p < l.size();) { const int n = int(std::min<size_t>(size_t(block), l.size() - p)); if (out) m.processOut(l.data() + p, r.data() + p, n); else m.processIn(l.data() + p, r.data() + p, n); p += size_t(n); } }

int main() {
    std::printf("[meters] K-weighting constants, LUFS reference signal, RMS, peak hold, robustness\n");
    { LevelMeters m; m.prepare(48000.0);                                        // coefficients published in ITU-R BS.1770 for 48 kHz
      CHECK(std::fabs(m.shelf().b0 - 1.53512485958697) < 1e-9 && std::fabs(m.shelf().b1 + 2.69169618940638) < 1e-9 && std::fabs(m.shelf().b2 - 1.19839281085285) < 1e-9, "shelf numerator %.12f %.12f %.12f", m.shelf().b0, m.shelf().b1, m.shelf().b2);
      CHECK(std::fabs(m.shelf().a1 + 1.69065929318241) < 1e-9 && std::fabs(m.shelf().a2 - 0.73248077421585) < 1e-9, "shelf denominator");
      CHECK(std::fabs(m.highpass().a1 + 1.99004745483398) < 1e-9 && std::fabs(m.highpass().a2 - 0.99007225036621) < 1e-9, "high-pass denominator %.12f %.12f", m.highpass().a1, m.highpass().a2); }
    for (double fs : {44100.0, 48000.0, 96000.0}) {                              // EBU Tech 3341: 1 kHz sine, -23 dBFS, both channels -> -23.0 LUFS (M and S)
        LevelMeters m; m.prepare(fs); const double amp = std::pow(10.0, -23.0 / 20.0);
        V s = sine(size_t(fs * 4), 1000.0, amp, fs); feed(m, s, s, 256, true);
        const auto f = m.frame();
        CHECK(std::fabs(f.lufsM[1] + 23.0) < 0.15 && std::fabs(f.lufsS[1] + 23.0) < 0.15, "fs=%g: LUFS-M %.3f LUFS-S %.3f (want -23.0)", fs, f.lufsM[1], f.lufsS[1]);
        CHECK(f.lufsM[0] <= -119.9f, "the IN meter must not see the OUT signal");
        CHECK(std::fabs(f.peak[OutL] + 23.0) < 0.05 && std::fabs(f.rms[OutL] + 26.01) < 0.05, "fs=%g: peak %.2f rms %.2f (want -23.00 / -26.01)", fs, f.peak[OutL], f.rms[OutL]);
    }
    { LevelMeters m; m.prepare(48000.0); V s = sine(48000, 440.0, 1.0, 48000.0); V z(48000, 0.f);        // L full-scale, R silent
      feed(m, s, z, 64); const auto f = m.frame();
      CHECK(std::fabs(f.peak[InL]) < 0.01 && f.peak[InR] <= -119.9f && std::fabs(f.rms[InL] + 3.01) < 0.05, "full-scale sine: peak %.2f rms %.2f", f.peak[InL], f.rms[InL]);
      CHECK(f.peak[OutL] <= -119.9f, "OUT untouched"); }
    { LevelMeters m; m.prepare(48000.0); m.setRmsWindowMs(50);                                           // custom RMS window
      V loud = sine(48000, 1000.0, 0.5, 48000.0), quiet = sine(2400, 1000.0, 0.05, 48000.0);             // 1 s loud then 50 ms quiet
      feed(m, loud, loud, 128); feed(m, quiet, quiet, 128);
      CHECK(std::fabs(m.frame().rms[InL] - (20 * std::log10(0.05 / std::sqrt(2.0)))) < 0.5, "50 ms window sees only the quiet tail: %.2f", m.frame().rms[InL]);
      m.setRmsWindowMs(3000); feed(m, V(100, 0.f), V(100, 0.f), 100);
      CHECK(std::fabs(m.frame().rms[InL] - 10 * std::log10(0.125 * 48000.0 / 144000.0)) < 0.3, "3 s window (zero-padded) holds 1 s of 0.125 power: %.2f (want -13.80)", m.frame().rms[InL]); }
    // ---- peak: instant attack, 50 ms hold, release in dB/s, exact peak watcher ------------------------------------------
    for (int block : {1, 7, 64, 512, 2048, 4096, 9000}) {                  // a transient is reported by the block that contains it, whatever the size
        LevelMeters m; m.prepare(48000.0); V s(20000, 0.f); s[3] = 0.8f;   // near the start of the first block
        float seen = -200.0f;
        for (size_t p = 0; p < s.size();) { const int n = int(std::min<size_t>(size_t(block), s.size() - p)); m.processIn(s.data() + p, s.data() + p, n); seen = std::max(seen, m.takeFrame().peak[InL]); p += size_t(n); }
        CHECK(std::fabs(seen - 20 * std::log10(0.8)) < 0.01, "block %d: attack reported %.3f dB (want -1.938)", block, seen);
    }
    { LevelMeters m; m.prepare(48000.0);                                    // timeline after a full-scale transient (decay set to 40 dB/s)
      V one(1, 1.0f), z(48, 0.f); m.processIn(one.data(), one.data(), 1); m.processIn(z.data(), z.data(), 47);
      const float atk = m.frame().peak[InL]; CHECK(std::fabs(atk) < 0.01, "full-scale transient reads %.3f dB right away", atk);
      auto at = [&](double ms) { LevelMeters k; k.prepare(48000.0); k.setPeakDecayDbPerSec(40.0); V o(1, 1.0f); k.processIn(o.data(), o.data(), 1); const int n = int(ms * 48.0) - 1; V q(size_t(n), 0.f); k.processIn(q.data(), q.data(), n); return k.frame().peak[InL]; };
      CHECK(std::fabs(at(40)) < 0.01, "still held at 40 ms: %.3f", at(40));
      CHECK(std::fabs(at(150) - (-40.0 * (0.150 - 0.050))) < 0.15, "150 ms: %.2f (want -4.0)", at(150));
      CHECK(std::fabs(at(550) - (-40.0 * (0.550 - 0.050))) < 0.15, "550 ms: %.2f (want -20.0)", at(550));
      const double slope = (at(850) - at(350)) / 0.5; CHECK(std::fabs(slope + 40.0) < 0.5, "release rate %.2f dB/s (want -40)", slope);
      CHECK(at(4000) <= -119.9f, "fully released after 4 s (%.2f)", at(4000));
      { LevelMeters d; d.prepare(48000.0); V o(1, 1.0f); d.processIn(o.data(), o.data(), 1); V q(size_t(1.050 * 48000), 0.f); d.processIn(q.data(), q.data(), int(q.size()));   // default rate: 24 dB/s
        CHECK(std::fabs(d.frame().peak[InL] - (-24.0 * 1.0)) < 0.3, "default release rate: %.2f dB after 1.05 s (want -24.0)", d.frame().peak[InL]); }
      LevelMeters f; f.prepare(48000.0); f.setPeakDecayDbPerSec(200.0); V o(1, 1.0f); f.processIn(o.data(), o.data(), 1); V q(size_t(0.250 * 48000), 0.f); f.processIn(q.data(), q.data(), int(q.size()));
      CHECK(std::fabs(f.frame().peak[InL] - (-200.0 * 0.200)) < 0.5, "decay 200 dB/s: %.2f (want -40)", f.frame().peak[InL]); }
    { LevelMeters m; m.prepare(48000.0); V big(1, 1.0f), small(1, 0.1f), z(2400, 0.f);   // a smaller peak never lowers the envelope
      m.processIn(big.data(), big.data(), 1); m.processIn(z.data(), z.data(), 480); m.processIn(small.data(), small.data(), 1);
      CHECK(m.frame().peak[InL] > -0.5f, "envelope after a smaller peak: %.2f", m.frame().peak[InL]);
      m.processIn(z.data(), z.data(), 2400); }
    { LevelMeters m; m.prepare(48000.0); V s(5000, 0.f), zr(5000, 0.f); s[1234] = 0.5f; m.processIn(s.data(), zr.data(), 5000);   // peak watcher: exact, independent of the decay (left channel only)
      V z(48000 * 3, 0.f); m.processIn(z.data(), z.data(), int(z.size()));
      CHECK(m.frame().peak[InL] < -60.0f, "envelope released after 3 s at the default 24 dB/s (%.1f)", m.frame().peak[InL]);
      CHECK(std::fabs(m.frame().peakMax[InL] - 20 * std::log10(0.5)) < 0.001 && m.frame().peakMax[InR] <= -119.9f, "peak watcher %.3f (want -6.021)", m.frame().peakMax[InL]);
      V t(100, 0.f), tz(100, 0.f); t[10] = 0.25f; m.processIn(t.data(), tz.data(), 100);
      CHECK(std::fabs(m.frame().peakMax[InL] - 20 * std::log10(0.5)) < 0.001, "a lower peak must not change the watcher");
      t[10] = 0.9f; m.processIn(t.data(), tz.data(), 100); CHECK(std::fabs(m.frame().peakMax[InL] - 20 * std::log10(0.9)) < 0.001, "a higher peak raises the watcher");
      m.resetPeakMax(); CHECK(m.frame().peakMax[InL] <= -119.9f, "reset"); }
    { LevelMeters m; m.prepare(48000.0); V bad(5000, 0.1f); bad[10] = NAN; bad[20] = INFINITY; feed(m, bad, bad, 77);
      const auto f = m.frame(); bool fin = true; for (int i = 0; i < 4; ++i) if (!std::isfinite(f.peak[i]) || !std::isfinite(f.rms[i])) fin = false; CHECK(fin && std::isfinite(f.lufsM[0]), "NaN / Inf input"); }
    { LevelMeters m; m.prepare(48000.0); std::mt19937 r(1); std::uniform_real_distribution<float> d(-1, 1); V a(300000), b(300000); for (auto& v : a) v = d(r); for (auto& v : b) v = d(r);
      feed(m, a, b, 513); const auto x = m.frame();                      // block-size independence
      LevelMeters m2; m2.prepare(48000.0); feed(m2, a, b, 17); const auto y = m2.frame();
      double e = 0; for (int i = 0; i < 4; ++i) e = std::max(e, double(std::fabs(x.rms[i] - y.rms[i]))); e = std::max(e, double(std::fabs(x.lufsS[0] - y.lufsS[0])));
      CHECK(e < 1e-4, "block size dependence %g", e); }
    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL METER TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
