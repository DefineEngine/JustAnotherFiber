// C-2: fiber-pool exhaustion must be detected, never a silent hang.
//
//  * NestedWaitsWithinBudgetComplete: legitimate nested fork/join that stays
//    within the pool budget completes -- the watchdog must NOT false-fire.
//  * ParkedFibersStarvePoolAborts: the only fiber is held by the parked root and
//    there is no Large fiber to borrow, so the forked children can never start.
//    The watchdog detects the livelock and aborts with a diagnostic. CTest's
//    per-test TIMEOUT is the backstop that guarantees this never just hangs.

#include <jaf/Jaf.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <vector>

namespace {

jaf::SchedulerOptions pool_opts(unsigned workers, std::size_t small, std::size_t large) {
    jaf::SchedulerOptions o;
    o.worker_count = workers;
    o.small_fibers = small;
    o.large_fibers = large;
    return o;
}

} // namespace

TEST(PoolExhaustion, NestedWaitsWithinBudgetComplete) {
    jaf::Scheduler sched(pool_opts(2, /*small*/ 16, /*large*/ 4));
    std::atomic<int> leaves{0};
    constexpr int kChildren = 3;
    constexpr int kGrand    = 3;

    sched.run([&] {
        jaf::Counter c;
        std::vector<jaf::Task> children;
        children.reserve(kChildren);
        for (int i = 0; i < kChildren; ++i) {
            children.emplace_back([&] {
                jaf::Counter cc;
                std::vector<jaf::Task> grand;
                grand.reserve(kGrand);
                for (int j = 0; j < kGrand; ++j) {
                    grand.emplace_back(
                        [&] { leaves.fetch_add(1, std::memory_order_relaxed); });
                }
                jaf::run(grand, &cc);
                jaf::wait_for_counter(&cc, 0);
            });
        }
        jaf::run(children, &c);
        jaf::wait_for_counter(&c, 0);
    });

    EXPECT_EQ(leaves.load(), kChildren * kGrand);
}

TEST(PoolExhaustionDeathTest, ParkedFibersStarvePoolAborts) {
    EXPECT_DEATH(
        {
            // small=1, large=0: the root takes the only fiber and parks on the
            // children's counter; the children can acquire nothing (no Small
            // free, no Large to borrow) and can never run -> permanent livelock.
            // Short watchdog window so the test fails fast instead of waiting the
            // 500 ms production default.
            jaf::SchedulerOptions opt = pool_opts(2, /*small*/ 1, /*large*/ 0);
            opt.pool_exhaustion_timeout = std::chrono::milliseconds(25);
            jaf::Scheduler sched(opt);
            sched.run([] {
                jaf::Counter c;
                std::vector<jaf::Task> children;
                children.emplace_back([] {});
                children.emplace_back([] {});
                jaf::run(children, &c);
                jaf::wait_for_counter(&c, 0);
            });
        },
        "fiber pool exhausted");
}
