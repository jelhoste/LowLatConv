// Aligned, zero-initialised, non-copyable array. Allocation happens only in resize()
// (never on the audio thread).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace llc {

template <typename T>
class AlignedArray {
    static_assert(std::is_trivial<T>::value, "AlignedArray holds trivial types only");
public:
    AlignedArray() = default;
    explicit AlignedArray(size_t n) { resize(n); }
    AlignedArray(const AlignedArray&) = delete;
    AlignedArray& operator=(const AlignedArray&) = delete;
    AlignedArray(AlignedArray&& o) noexcept : p_(o.p_), n_(o.n_) { o.p_ = nullptr; o.n_ = 0; }
    AlignedArray& operator=(AlignedArray&& o) noexcept {
        if (this != &o) { release(); p_ = o.p_; n_ = o.n_; o.p_ = nullptr; o.n_ = 0; }
        return *this;
    }
    ~AlignedArray() { release(); }

    void resize(size_t n) {
        release();
        if (n == 0) return;
        // Manual 64-byte alignment: the C++17 aligned operator new does not exist before macOS 10.13 and the
        // plugin targets older systems. The raw pointer is stored just before the aligned block.
        const size_t bytes = n * sizeof(T);
        void* raw = std::malloc(bytes + 64 + sizeof(void*));
        if (!raw) std::abort();                                   // out of memory is fatal (as an uncaught bad_alloc was)
        const std::uintptr_t addr = (reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*) + 63u) & ~std::uintptr_t(63u);
        reinterpret_cast<void**>(addr)[-1] = raw;
        p_ = reinterpret_cast<T*>(addr);
        n_ = n;
        std::memset(static_cast<void*>(p_), 0, bytes);
    }
    void zero() { if (p_) std::memset(static_cast<void*>(p_), 0, n_ * sizeof(T)); }

    T* data() { return p_; }
    const T* data() const { return p_; }
    size_t size() const { return n_; }
    T& operator[](size_t i) { return p_[i]; }
    const T& operator[](size_t i) const { return p_[i]; }

private:
    void release() {
        if (p_) std::free(reinterpret_cast<void**>(p_)[-1]);
        p_ = nullptr; n_ = 0;
    }
    T* p_ = nullptr;
    size_t n_ = 0;
};

} // namespace llc
