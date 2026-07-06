// F-7: jaf::parallel_for -- chunked parallel loop against a stack Counter.

#include <jaf/Jaf.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <vector>

namespace {

jaf::SchedulerOptions with_workers(unsigned n) {
    jaf::SchedulerOptions opt;
    opt.worker_count = n;
    return opt;
}

} // namespace

TEST(ParallelFor, SumEqualsSerialAutoGrain) {
    jaf::Scheduler sched(with_workers(4));
    std::atomic<long long> sum{0};
    constexpr int N = 100000;
    sched.run([&] {
        jaf::parallel_for(0, N, [&](int i) { sum.fetch_add(i, std::memory_order_relaxed); });
    });
    EXPECT_EQ(sum.load(), static_cast<long long>(N) * (N - 1) / 2);
}

TEST(ParallelFor, CoversRangeExactlyOnceExplicitGrain) {
    jaf::Scheduler sched(with_workers(4));
    constexpr int N = 5000;
    std::vector<std::atomic<int>> hits(N);
    for (auto& h : hits) h.store(0, std::memory_order_relaxed);

    sched.run([&] {
        jaf::parallel_for(0, N, /*grain*/ 64,
                          [&](int i) { hits[i].fetch_add(1, std::memory_order_relaxed); });
    });

    int wrong = 0;
    for (int i = 0; i < N; ++i) {
        if (hits[i].load(std::memory_order_relaxed) != 1) ++wrong;
    }
    EXPECT_EQ(wrong, 0);   // every index hit exactly once: no gaps, no overlap
}

TEST(ParallelFor, EmptyAndReversedRangesAreNoOps) {
    jaf::Scheduler sched(with_workers(2));
    std::atomic<int> calls{0};
    sched.run([&] {
        jaf::parallel_for(5, 5, [&](int) { calls.fetch_add(1, std::memory_order_relaxed); });
        jaf::parallel_for(10, 5, [&](int) { calls.fetch_add(1, std::memory_order_relaxed); });
    });
    EXPECT_EQ(calls.load(), 0);
}
