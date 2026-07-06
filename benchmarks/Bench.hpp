#pragma once

// =============================================================================
//  benchmarks/bench.hpp -- a tiny, dependency-free micro-benchmark harness.
//
//  Deliberately minimal: a steady_clock timer with a warmup pass that reports
//  nanoseconds per operation. We roll our own (rather than pull in a benchmark
//  library) per the project's "we can benchmark ourselves" goal.
// =============================================================================

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace jaf::bench {

// T-6: prevent the optimizer from discarding a computed value. The previous
// version only FORMED a volatile pointer (no access) -- a no-op the compiler
// could still elide around. Dereferencing it forces a real volatile read.
template <class T>
inline void keep(T const& value) noexcept {
    volatile T const* sink = &value;
    (void)*sink;
}

// T-6: print once, so nobody mistakes debug-build numbers for real ones.
inline void debug_banner_once() {
#ifndef NDEBUG
    static bool printed = false;
    if (!printed) {
        std::printf("*** DEBUG build -- benchmark numbers are MEANINGLESS ***\n");
        printed = true;
    }
#endif
}

// Runs `fn` `iters` times per repetition, over K reps (after a warmup), and
// reports min / median / spread of ns/op. Single means cannot distinguish a
// 10-20%% regression from the scheduler's us-scale noise; min+median can. Returns
// the median. `fn` performs one "operation" per call.
template <class Fn>
double run(char const* name, std::uint64_t iters, Fn&& fn) {
    using clock = std::chrono::steady_clock;
    debug_banner_once();

    constexpr int kReps = 11;
    std::uint64_t const warmup = iters / 10 + 1;
    for (std::uint64_t i = 0; i < warmup; ++i) fn();

    double samples[kReps];
    for (int r = 0; r < kReps; ++r) {
        auto const t0 = clock::now();
        for (std::uint64_t i = 0; i < iters; ++i) fn();
        auto const t1 = clock::now();
        auto const ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        samples[r] = iters ? static_cast<double>(ns) / static_cast<double>(iters) : 0.0;
    }

    // Insertion sort (kReps is tiny) -> min, median, max.
    for (int i = 1; i < kReps; ++i) {
        double const v = samples[i];
        int j = i - 1;
        while (j >= 0 && samples[j] > v) { samples[j + 1] = samples[j]; --j; }
        samples[j + 1] = v;
    }
    double const mn  = samples[0];
    double const med = samples[kReps / 2];
    double const mx  = samples[kReps - 1];
    double const spread = mx > 0.0 ? (mx - mn) / mx * 100.0 : 0.0;

    std::printf("%-32s %10llu it   min %9.2f  med %9.2f  spread %5.1f%%  ns/op\n",
                name, static_cast<unsigned long long>(iters), mn, med, spread);
    return med;
}

} // namespace jaf::bench
