// RAII: flush denormals to zero on the calling (audio) thread for the scope's duration.
// x86 (SSE) path is tested; the AArch64 path is untested.
#pragma once
#if defined(__SSE2__) || defined(_M_X64)
  #include <xmmintrin.h>
  #include <pmmintrin.h>
#endif
namespace llc {
class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() {
#if defined(__SSE2__) || defined(_M_X64)
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ | 0x8040u);                 // FTZ | DAZ
#elif defined(__aarch64__)
        unsigned long long r; asm volatile("mrs %0, fpcr" : "=r"(r)); saved_ = r;
        r |= (1ull << 24);                            // FZ
        asm volatile("msr fpcr, %0" :: "r"(r));
#endif
    }
    ~ScopedFlushDenormals() {
#if defined(__SSE2__) || defined(_M_X64)
        _mm_setcsr(saved_);
#elif defined(__aarch64__)
        asm volatile("msr fpcr, %0" :: "r"(saved_));
#endif
    }
    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;
private:
    unsigned long long saved_ = 0;
};
} // namespace llc
