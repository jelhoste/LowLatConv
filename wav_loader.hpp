// Robust WAV -> IRBuffer loader on top of dr_wav (public domain / MIT-0).
// Requires third_party/dr_wav/dr_wav_impl.cpp to be linked once.
//  * the file is read into memory first (UTF-8 paths work on every OS, no fopen encoding issues)
//  * hard limits on file size, channels, sample rate and frame count (memory-safe on hostile files)
//  * decoding is chunked, so memory follows the data really present (truncated files are accepted
//    up to the last complete frame)
//  * 8/16/24/32-bit PCM, 32/64-bit float, extensible and Wave64 are handled by dr_wav
#pragma once
#include "ir_processor.hpp"
#include <cstdio>
#include <string>
#include <vector>
#include "dr_wav.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace llc {

enum class WavStatus { Ok, CannotOpen, TooLarge, Invalid, Empty, UnsupportedChannels, UnsupportedRate };

inline const char* wavStatusText(WavStatus s) {
    switch (s) {
    case WavStatus::Ok: return "ok";
    case WavStatus::CannotOpen: return "cannot open file";
    case WavStatus::TooLarge: return "file too large";
    case WavStatus::Invalid: return "not a valid WAV file";
    case WavStatus::Empty: return "no audio data";
    case WavStatus::UnsupportedChannels: return "unsupported channel count";
    case WavStatus::UnsupportedRate: return "unsupported sample rate";
    }
    return "?";
}

struct WavLimits {
    size_t maxFileBytes = size_t(256) << 20;      // 256 MiB
    size_t maxFrames    = size_t(1) << 24;        // ~6 min at 48 kHz
    unsigned maxChannels = 8;
};

inline WavStatus loadWavMemory(const void* data, size_t size, IRBuffer& out, const WavLimits& lim = {}) {
    out = IRBuffer{};
    if (!data || size < 12) return WavStatus::Invalid;
    if (size > lim.maxFileBytes) return WavStatus::TooLarge;
    drwav wav;
    if (!drwav_init_memory(&wav, data, size, nullptr)) return WavStatus::Invalid;
    struct Guard { drwav* w; ~Guard() { drwav_uninit(w); } } guard{&wav};
    if (wav.channels == 0 || wav.channels > lim.maxChannels) return WavStatus::UnsupportedChannels;
    if (wav.sampleRate < 1000 || wav.sampleRate > 768000) return WavStatus::UnsupportedRate;

    const unsigned nch = wav.channels;
    std::vector<std::vector<float>> ch(nch);
    std::vector<float> buf(size_t(65536) * nch);
    size_t total = 0;
    for (;;) {
        const drwav_uint64 got = drwav_read_pcm_frames_f32(&wav, 65536, buf.data());
        if (got == 0) break;
        if (total + size_t(got) > lim.maxFrames) return WavStatus::TooLarge;
        for (unsigned c = 0; c < nch; ++c) {
            auto& v = ch[c]; v.resize(total + size_t(got));
            for (size_t i = 0; i < size_t(got); ++i) {
                const float s = buf[i * nch + c];
                v[total + i] = (s - s == 0.0f) ? s : 0.0f;          // NaN / Inf -> 0
            }
        }
        total += size_t(got);
    }
    if (total == 0) return WavStatus::Empty;
    out.sampleRate = double(wav.sampleRate);
    out.ch = std::move(ch);
    return WavStatus::Ok;
}

// Opens a UTF-8 path on every platform (on Windows the narrow fopen would use the ANSI code page and fail on
// accented folder names).
inline std::FILE* openUtf8(const std::string& utf8Path) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, nullptr, 0);
    if (n <= 0) return nullptr;
    std::vector<wchar_t> wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, wide.data(), n);
    return _wfopen(wide.data(), L"rb");
#else
    return std::fopen(utf8Path.c_str(), "rb");
#endif
}

inline WavStatus loadWavFile(const std::string& utf8Path, IRBuffer& out, const WavLimits& lim = {}) {
    out = IRBuffer{};
    std::FILE* f = openUtf8(utf8Path);
    if (!f) return WavStatus::CannotOpen;
    struct Closer { std::FILE* f; ~Closer() { std::fclose(f); } } closer{f};
    if (std::fseek(f, 0, SEEK_END) != 0) return WavStatus::CannotOpen;
    const long sz = std::ftell(f);                                         // fails (-1) on directories
    if (sz < 0) return WavStatus::CannotOpen;
    if (static_cast<size_t>(sz) > lim.maxFileBytes) return WavStatus::TooLarge;
    std::rewind(f);
    std::vector<char> bytes(static_cast<size_t>(sz), 0);
    if (sz > 0 && std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) return WavStatus::CannotOpen;
    return loadWavMemory(bytes.data(), bytes.size(), out, lim);
}

} // namespace llc
