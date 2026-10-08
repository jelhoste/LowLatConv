// Load / worst-case measurements. Mean = best of 5 repetitions; "structural spike" = median
// time of the calls on which the largest partition fires (robust to OS scheduling noise).
#include "convolver.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
using namespace llc;
static double now_us() { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int main() {
    std::printf("FFT backend: %s\n", Convolver::fftName());
    std::mt19937 rng(5); std::uniform_real_distribution<float> d(-1, 1);
    for (size_t L : {2048u, 8192u, 48000u, 96000u})
        for (int block : {32, 64, 256}) {
            const int maxP = 1024, head = 64;
            std::vector<float> h(L); for (auto& v : h) v = d(rng) * 0.1f;
            ConvolverConfig cfg; cfg.headSize = head; cfg.maxPartition = maxP;
            Convolver c; c.prepare(h.data(), L, cfg);
            AlignedArray<float> x{static_cast<size_t>(block)}; AlignedArray<float> y{static_cast<size_t>(block)};
            for (size_t i = 0; i < size_t(block); ++i) x[i] = d(rng);
            const double budget = 1e6 * block / 48000.0;
            double best = 1e12;
            for (int rep = 0; rep < 5; ++rep) {
                const double t0 = now_us();
                for (int i = 0; i < 20000; ++i) c.process(x.data(), y.data(), block);
                best = std::min(best, (now_us() - t0) / 20000.0);
            }
            std::vector<double> boundary;
            const int per = std::max(1, maxP / block);
            for (int i = 0; i < 6000; ++i) {
                const double t0 = now_us(); c.process(x.data(), y.data(), block); const double us = now_us() - t0;
                if (((i + 1) % per) == 0) boundary.push_back(us);
            }
            std::sort(boundary.begin(), boundary.end());
            std::printf("IR=%6zu block=%3d stages=%zu | mean %6.2f us (%.3f%% CPU) | structural spike %6.1f us (%.1f%% of %.0f us budget)\n",
                        L, block, c.stageCount(), best, 100.0 * best / budget, boundary[boundary.size() / 2], 100.0 * boundary[boundary.size() / 2] / budget, budget);
        }
}
