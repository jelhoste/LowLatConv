#include "wav_loader.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
using namespace llc;
static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

typedef std::vector<uint8_t> Bytes;
static void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
static void put16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
static void tag(Bytes& b, const char* t) { b.insert(b.end(), t, t + 4); }

// kind: 1 = PCM, 3 = float, 0xFFFE = extensible (PCM or float inside)
static Bytes makeWav(const std::vector<std::vector<double>>& chans, uint32_t sr, int bits, uint16_t kind, bool listChunk = false, bool truncate = false, bool extFloat = false) {
    const size_t nch = chans.size(), n = chans[0].size(), bps = size_t(bits / 8);
    Bytes data;
    for (size_t i = 0; i < n; ++i) for (size_t c = 0; c < nch; ++c) {
        const double v = chans[c][i];
        if ((kind == 3) || (kind == 0xFFFE && extFloat)) {
            if (bits == 32) { float f = float(v); uint32_t u; std::memcpy(&u, &f, 4); put32(data, u); }
            else { uint64_t u; std::memcpy(&u, &v, 8); put32(data, uint32_t(u)); put32(data, uint32_t(u >> 32)); }
        } else if (bits == 8) data.push_back(uint8_t(std::lround(v * 127.0) + 128));
        else if (bits == 16) put16(data, uint16_t(int16_t(std::lround(v * 32767.0))));
        else if (bits == 24) { int32_t s = int32_t(std::lround(v * 8388607.0)); data.push_back(uint8_t(s)); data.push_back(uint8_t(s >> 8)); data.push_back(uint8_t(s >> 16)); }
        else put32(data, uint32_t(int32_t(std::lround(v * 2147483647.0))));
    }
    Bytes fmt; const bool ext = kind == 0xFFFE;
    put16(fmt, kind); put16(fmt, uint16_t(nch)); put32(fmt, sr); put32(fmt, uint32_t(sr * nch * bps)); put16(fmt, uint16_t(nch * bps)); put16(fmt, uint16_t(bits));
    if (ext) { put16(fmt, 22); put16(fmt, uint16_t(bits)); put32(fmt, 3); put16(fmt, extFloat ? 3 : 1); const uint8_t g[14] = {0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71}; fmt.insert(fmt.end(), g, g + 14); }
    Bytes out; tag(out, "RIFF"); put32(out, 0); tag(out, "WAVE");
    tag(out, "fmt "); put32(out, uint32_t(fmt.size())); out.insert(out.end(), fmt.begin(), fmt.end());
    if (listChunk) { tag(out, "LIST"); put32(out, 5); const char* s = "INFOx"; out.insert(out.end(), s, s + 5); out.push_back(0); }   // odd size + pad byte
    tag(out, "data"); put32(out, uint32_t(data.size()));
    if (truncate) data.resize(data.size() - data.size() / 3 - 1);
    out.insert(out.end(), data.begin(), data.end());
    const uint32_t riff = uint32_t(out.size() - 8); for (int i = 0; i < 4; ++i) out[4 + size_t(i)] = uint8_t(riff >> (8 * i));
    return out;
}

static std::vector<std::vector<double>> testSignal(size_t nch, size_t n) {
    std::mt19937 rng(3); std::uniform_real_distribution<double> d(-0.9, 0.9);
    std::vector<std::vector<double>> v(nch, std::vector<double>(n));
    for (auto& c : v) for (auto& x : c) x = d(rng);
    return v;
}

static void testFormats() {
    std::printf("[wav] formats: PCM 8/16/24/32, float 32/64, extensible, extra chunks\n");
    struct F { int bits; uint16_t kind; bool ext; double tol; } fs[] = {   // output is float32: 32-bit sources are limited by float precision (~6e-8)
        {8, 1, false, 1.0 / 64}, {16, 1, false, 1e-4}, {24, 1, false, 1e-6}, {32, 1, false, 1e-7}, {32, 3, false, 1e-7}, {64, 3, false, 1e-7},
        {24, 0xFFFE, false, 1e-6}, {32, 0xFFFE, true, 1e-7}};
    for (size_t nch : {1u, 2u, 4u}) for (auto f : fs) {
        auto sig = testSignal(nch, 3000);
        Bytes w = makeWav(sig, 44100, f.bits, f.kind, true, false, f.ext);
        IRBuffer ir; const WavStatus st = loadWavMemory(w.data(), w.size(), ir);
        CHECK(st == WavStatus::Ok, "%zu ch %d bit kind %x: %s", nch, f.bits, f.kind, wavStatusText(st));
        if (st != WavStatus::Ok) continue;
        double err = 0; for (size_t c = 0; c < nch; ++c) for (size_t i = 0; i < 3000; ++i) err = std::max(err, std::fabs(double(ir.ch[c][i]) - sig[c][i]));
        CHECK(ir.ch.size() == nch && ir.length() == 3000 && ir.sampleRate == 44100.0 && err < f.tol, "%zu ch %d bit kind %x: ch=%zu len=%zu sr=%g err=%g", nch, f.bits, f.kind, ir.ch.size(), ir.length(), ir.sampleRate, err);
    }
}

static void testBad() {
    std::printf("[wav] invalid / hostile input\n");
    IRBuffer ir;
    CHECK(loadWavMemory(nullptr, 0, ir) == WavStatus::Invalid, "null");
    Bytes junk(5000); std::mt19937 rng(1); for (auto& b : junk) b = uint8_t(rng());
    CHECK(loadWavMemory(junk.data(), junk.size(), ir) == WavStatus::Invalid, "random bytes");
    Bytes hdr = makeWav(testSignal(1, 100), 48000, 16, 1);
    CHECK(loadWavMemory(hdr.data(), 20, ir) != WavStatus::Ok, "truncated header");
    Bytes empty = makeWav(testSignal(1, 1), 48000, 16, 1); empty.resize(empty.size() - 2);
    CHECK(loadWavMemory(empty.data(), empty.size(), ir) != WavStatus::Ok, "no samples");
    Bytes tr = makeWav(testSignal(2, 3000), 48000, 16, 1, false, true);
    const WavStatus st = loadWavMemory(tr.data(), tr.size(), ir);
    CHECK(st == WavStatus::Ok && ir.length() > 1000 && ir.length() < 3000, "truncated data: %s len=%zu", wavStatusText(st), ir.length());
    Bytes many = makeWav(testSignal(9, 100), 48000, 16, 1);
    CHECK(loadWavMemory(many.data(), many.size(), ir) == WavStatus::UnsupportedChannels, "9 channels");
    Bytes lowSr = makeWav(testSignal(1, 100), 100, 16, 1);
    CHECK(loadWavMemory(lowSr.data(), lowSr.size(), ir) == WavStatus::UnsupportedRate, "100 Hz sample rate");
    WavLimits tiny; tiny.maxFrames = 500; Bytes big = makeWav(testSignal(1, 2000), 48000, 16, 1);
    CHECK(loadWavMemory(big.data(), big.size(), ir, tiny) == WavStatus::TooLarge, "frame limit");
    tiny = WavLimits{}; tiny.maxFileBytes = 1000;
    CHECK(loadWavMemory(big.data(), big.size(), ir, tiny) == WavStatus::TooLarge, "byte limit");
    Bytes lie = makeWav(testSignal(1, 100), 48000, 16, 1);
    lie[40] = lie[41] = lie[42] = 0xFF; lie[43] = 0x7F;                          // data size claims ~2 GB
    const WavStatus st2 = loadWavMemory(lie.data(), lie.size(), ir);
    CHECK(st2 == WavStatus::Ok && ir.length() == 100, "oversized data chunk: %s len=%zu", wavStatusText(st2), ir.length());
    CHECK(loadWavFile("/nonexistent/dir/file.wav", ir) == WavStatus::CannotOpen, "missing file");
    CHECK(loadWavFile("/tmp", ir) != WavStatus::Ok, "directory");
}

static void testFileAndFuzz() {
    std::printf("[wav] file round trip and 4000 random corruptions (no crash, no hang)\n");
    auto sig = testSignal(2, 4000); Bytes w = makeWav(sig, 48000, 24, 1, true);
    { std::ofstream f("llc_test_ir.wav", std::ios::binary); f.write(reinterpret_cast<const char*>(w.data()), std::streamsize(w.size())); }
    IRBuffer ir; CHECK(loadWavFile("llc_test_ir.wav", ir) == WavStatus::Ok && ir.length() == 4000 && ir.ch.size() == 2, "file load");
    std::mt19937 rng(77); int ok = 0, bad = 0;
    for (int it = 0; it < 4000; ++it) {
        Bytes m = w;
        const int edits = 1 + int(rng() % 6);
        for (int e = 0; e < edits; ++e) m[rng() % std::min<size_t>(m.size(), 80)] = uint8_t(rng());       // corrupt the header area
        if (rng() % 4 == 0) m.resize(rng() % m.size());
        WavLimits lim; lim.maxFrames = 1 << 20;
        IRBuffer o; const WavStatus st = loadWavMemory(m.data(), m.size(), o, lim);
        if (st == WavStatus::Ok) { ++ok; bool fin = true; for (auto& c : o.ch) for (float v : c) if (!std::isfinite(v)) fin = false; CHECK(fin, "non-finite sample"); CHECK(o.length() > 0 && o.ch.size() >= 1 && o.ch.size() <= 8, "bad shape"); }
        else ++bad;
    }
    std::printf("  accepted %d, rejected %d\n", ok, bad);
}

int main() {
    testFormats(); testBad(); testFileAndFuzz();
    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL WAV TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
