// Glitch-free hot-swapping of stereo engines (impulse responses).
//
//  message/loader thread : build a StereoEngine (allocates) -> LiveConvolver::load()
//  audio thread          : LiveConvolver::process()  (no allocation, no locks, no frees)
//
// A newly loaded engine fades in over `fadeSamples` while the previous one fades out.
// Old engines are handed back through a lock-free slot and freed by collectRetired()
// on the message thread, never on the audio thread. With no engine loaded the signal
// is passed through unchanged (dry).
#pragma once
#include "stereo_engine.hpp"
#include <atomic>
#include <memory>

namespace llc {

class LiveConvolver {
public:
    // silentWhenEmpty: with no IR loaded output silence instead of passing the input through.
    explicit LiveConvolver(int fadeSamples = 1024, bool silentWhenEmpty = false) : fadeLen_(std::max(1, fadeSamples)), silent_(silentWhenEmpty) {
        for (auto* s : {&sA_[0], &sA_[1], &sB_[0], &sB_[1]}) s->resize(kChunk);
    }
    ~LiveConvolver() {
        delete pending_.exchange(nullptr);
        delete retired_.exchange(nullptr);
        delete active_; delete fading_; delete parked_;
    }
    LiveConvolver(const LiveConvolver&) = delete;
    LiveConvolver& operator=(const LiveConvolver&) = delete;

    // ---- message thread -------------------------------------------------------------
    // Takes ownership. A previously queued, not-yet-installed engine is simply replaced.
    void load(std::unique_ptr<StereoEngine> next) {
        if (!next || !next->ready()) return;
        delete pending_.exchange(next.release(), std::memory_order_acq_rel);
    }
    // Removes the IR (fades back to dry).
    void unload() { unloadRequested_.store(true, std::memory_order_release); }
    // Call periodically (e.g. from a timer / after load) to free engines the audio thread is done with.
    void collectRetired() { delete retired_.exchange(nullptr, std::memory_order_acq_rel); }

    // ---- audio thread ----------------------------------------------------------------
    void process(const float* inL, const float* inR, float* outL, float* outR, int n) {
        takePendingIfIdle();
        while (n > 0) {
            const int len = std::min(n, kChunk);
            processChunk(inL, inR, outL, outR, len);
            inL += len; inR += len; outL += len; outR += len; n -= len;
        }
        finishFadeIfDone();
    }
    void reset() { if (active_) active_->reset(); if (fading_) fading_->reset(); }
    bool hasIR() const { return active_ != nullptr; }          // audio-thread view

private:
    static constexpr int kChunk = 512;

    void takePendingIfIdle() {
        if (fading_ || parked_) { finishFadeIfDone(); if (fading_ || parked_) return; }
        StereoEngine* p = pending_.exchange(nullptr, std::memory_order_acq_rel);
        if (p) {
            fading_ = active_;          // may be null (fade from dry)
            active_ = p;
            fadePos_ = 0;
            crossfading_ = true;
        } else if (unloadRequested_.exchange(false, std::memory_order_acq_rel) && active_) {
            fading_ = active_;          // fade to dry
            active_ = nullptr;
            fadePos_ = 0;
            crossfading_ = true;
        }
    }

    void processChunk(const float* inL, const float* inR, float* outL, float* outR, int len) {
        if (!crossfading_) {
            if (active_) active_->process(inL, inR, outL, outR, len);
            else if (silent_) { std::memset(outL, 0, size_t(len) * sizeof(float)); std::memset(outR, 0, size_t(len) * sizeof(float)); }
            else { if (outL != inL) std::memcpy(outL, inL, size_t(len) * sizeof(float)); if (outR != inR) std::memcpy(outR, inR, size_t(len) * sizeof(float)); }
            return;
        }
        float* aL = sA_[0].data(); float* aR = sA_[1].data(); float* bL = sB_[0].data(); float* bR = sB_[1].data();
        if (active_) active_->process(inL, inR, aL, aR, len);                         // incoming (new)
        else if (silent_) { std::memset(aL, 0, size_t(len) * sizeof(float)); std::memset(aR, 0, size_t(len) * sizeof(float)); }
        else { std::memcpy(aL, inL, size_t(len) * sizeof(float)); std::memcpy(aR, inR, size_t(len) * sizeof(float)); }
        if (fading_) fading_->process(inL, inR, bL, bR, len);                        // outgoing (old)
        else if (silent_) { std::memset(bL, 0, size_t(len) * sizeof(float)); std::memset(bR, 0, size_t(len) * sizeof(float)); }
        else { std::memcpy(bL, inL, size_t(len) * sizeof(float)); std::memcpy(bR, inR, size_t(len) * sizeof(float)); }
        const float inv = 1.0f / float(fadeLen_);
        for (int i = 0; i < len; ++i) {
            const float g = float(std::min(fadePos_ + i, fadeLen_)) * inv;
            outL[i] = bL[i] + (aL[i] - bL[i]) * g;
            outR[i] = bR[i] + (aR[i] - bR[i]) * g;
        }
        fadePos_ = std::min(fadePos_ + len, fadeLen_);
    }

    void finishFadeIfDone() {
        if (crossfading_ && fadePos_ >= fadeLen_) { crossfading_ = false; parked_ = fading_; fading_ = nullptr; }
        if (parked_) {   // hand the old engine back for destruction off the audio thread
            StereoEngine* expected = nullptr;
            if (retired_.compare_exchange_strong(expected, parked_, std::memory_order_acq_rel)) parked_ = nullptr;
        }
    }

    const int fadeLen_;
    const bool silent_;
    StereoEngine* active_ = nullptr;
    StereoEngine* fading_ = nullptr;   // outgoing engine while crossfading
    StereoEngine* parked_ = nullptr;   // finished engine waiting for a free hand-off slot
    int fadePos_ = 0;
    bool crossfading_ = false;
    std::atomic<StereoEngine*> pending_{nullptr};
    std::atomic<StereoEngine*> retired_{nullptr};
    std::atomic<bool> unloadRequested_{false};
    AlignedArray<float> sA_[2], sB_[2];
};

} // namespace llc
