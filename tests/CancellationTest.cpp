// F-9: cooperative cancellation via jaf::CancellationToken.

#include <jaf/Jaf.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <vector>

namespace {

jaf::SchedulerOptions with_workers(unsigned n) {
    jaf::SchedulerOptions opt;
    opt.worker_count = n;
    return opt;
}

} // namespace

TEST(Cancellation, CancelledBeforeStartSkippedButCounterDrains) {
    jaf::Scheduler sched(with_workers(4));
    std::atomic<int> ran{0};

    sched.run([&] {
        jaf::CancellationToken tok;
        tok.cancel();                      // cancel before submitting: all skipped
        jaf::Counter c;
        std::vector<jaf::Task> jobs;
        jobs.reserve(50);
        for (int i = 0; i < 50; ++i) {
            jobs.emplace_back([&] { ran.fetch_add(1, std::memory_order_relaxed); });
        }
        jaf::run(jobs, &c, tok);
        jaf::wait_for_counter(&c, 0);      // MUST NOT hang despite cancellation
    });

    EXPECT_EQ(ran.load(), 0);   // every job's task skipped
}

TEST(Cancellation, RunningJobObservesItsToken) {
    jaf::Scheduler sched(with_workers(2));
    std::atomic<bool> saw_cancel{false};

    sched.run([&] {
        jaf::CancellationToken tok;
        jaf::Counter c;
        std::vector<jaf::Task> jobs;
        jobs.emplace_back([&] {
            tok.cancel();                                   // cancel from inside
            saw_cancel.store(jaf::this_job::cancelled(),    // observed cooperatively
                             std::memory_order_relaxed);
        });
        jaf::run(jobs, &c, tok);
        jaf::wait_for_counter(&c, 0);
    });

    EXPECT_TRUE(saw_cancel.load());
}
