// C-1 regression: Counter completion-window use-after-free.
//
// The hazard: a waiter observes "satisfied" and lets a stack-allocated Counter's
// frame die while another completer is still inside CounterCompleteOne touching
// that Counter. This test hammers the fork/join primitive so that a completer's
// critical section overlaps the destruction+reuse of the previous iteration's
// stack Counter. It is designed to be run under ASan and a data-race detector in
// CI, where the default iteration count should be raised via -DJAF_STRESS_ITERS.
//
// Correctness here means: no crash, no torn count. The real teeth are ASan/TSan
// clean under a high iteration count -- which we cannot run in this environment,
// so CI owns the acceptance (see docs/spec.md C-1).

#include <jaf/Jaf.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// Overridable so default `ctest` stays quick while CI/ASan can crank it to >=1e6.
#ifndef JAF_STRESS_ITERS
#define JAF_STRESS_ITERS 50000
#endif

namespace {

jaf::SchedulerOptions with_workers(unsigned n) {
    jaf::SchedulerOptions opt;
    opt.worker_count = n;
    return opt;
}

} // namespace

// Tight fork/join loop: each iteration builds a FRESH stack Counter, runs a small
// batch against it, and joins. When wait_for_counter returns and the frame rolls
// into the next iteration, the previous Counter's stack bytes are reused -- which
// is exactly when a lagging completer's still-open critical section would corrupt
// them under the pre-C-1 code. Jobs count varies 1..4 to also exercise the
// single-completer path that the old lock-free fast path raced.
TEST(CounterStress, RepeatedForkJoinNoUseAfterFree) {
    jaf::Scheduler sched(with_workers(4));

    std::atomic<std::int64_t> total{0};
    std::int64_t              expected = 0;

    // Precompute expected so the assertion is exact regardless of interleaving.
    for (int it = 0; it < JAF_STRESS_ITERS; ++it) expected += 1 + (it % 4);

    sched.run([&] {
        for (int it = 0; it < JAF_STRESS_ITERS; ++it) {
            jaf::Counter c;                       // fresh, on this fiber's stack
            const int n = 1 + (it % 4);           // 1..4 completers
            std::vector<jaf::Task> jobs;
            jobs.reserve(static_cast<std::size_t>(n));
            for (int j = 0; j < n; ++j) {
                jobs.emplace_back(
                    [&] { total.fetch_add(1, std::memory_order_relaxed); });
            }
            jaf::run(jobs, &c);
            jaf::wait_for_counter(&c, 0);
        }
    });

    EXPECT_EQ(total.load(), expected);
}

// Targeted two-completer case (value == 2, one parked waiter): the classic race
// where completer B drives the value to 0 and wakes the waiter while completer A
// has decremented but not yet re-touched the Counter. Repeated so the window is
// hit many times across workers.
TEST(CounterStress, TwoCompletersOneWaiter) {
    jaf::Scheduler sched(with_workers(4));

    constexpr int kReps = JAF_STRESS_ITERS / 4 + 1;
    std::atomic<std::int64_t> total{0};

    sched.run([&] {
        for (int it = 0; it < kReps; ++it) {
            jaf::Counter c;
            std::vector<jaf::Task> jobs;
            jobs.reserve(2);
            jobs.emplace_back([&] { total.fetch_add(1, std::memory_order_relaxed); });
            jobs.emplace_back([&] { total.fetch_add(1, std::memory_order_relaxed); });
            jaf::run(jobs, &c);
            jaf::wait_for_counter(&c, 0);
        }
    });

    EXPECT_EQ(total.load(), static_cast<std::int64_t>(kReps) * 2);
}
