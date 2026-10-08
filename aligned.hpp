// Aligned, zero-initialised, non-copyable array. Allocation happens only in resize()
// (never on the audio thread).
#pragma once
#include <cstddef>
#include <cstring>
#include <new>
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
        p_ = static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t(64)));
        n_ = n;
        std::memset(static_cast<void*>(p_), 0, n * sizeof(T));
    }
    void zero() { if (p_) std::memset(static_cast<void*>(p_), 0, n_ * sizeof(T)); }

    T* data() { return p_; }
    const T* data() const { return p_; }
    size_t size() const { return n_; }
    T& operator[](size_t i) { return p_[i]; }
    const T& operator[](size_t i) const { return p_[i]; }

private:
    void release() {
        if (p_) ::operator delete(static_cast<void*>(p_), std::align_val_t(64));
        p_ = nullptr; n_ = 0;
    }
    T* p_ = nullptr;
    size_t n_ = 0;
};

} // namespace llc
