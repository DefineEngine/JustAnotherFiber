// F-10: timed / try wait for fibers -- wait_for_counter_until, try_wait_for_counter.

#include <jaf/Jaf.hpp>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>

namespace {

jaf::SchedulerOptions with_workers(unsigned n) {
    jaf::SchedulerOptions opt;
    opt.worker_count = n;
    return opt;
}

using namespace std::chrono_literals;

} // namespace

TEST(TimedWait, TimesOutOnNeverSatisfied) {
    jaf::Scheduler sched(with_workers(2));
    std::atomic<bool> timed_out{false};
    sched.run([&] {
        jaf::Counter c{1};   // never reaches 0
        const bool ok = jaf::wait_for_counter_until(&c, 0, std::chrono::steady_clock::now() + 50ms);
        timed_out.store(!ok);
        // c is unlinked from the wait-list on timeout, so its dtor is clean.
    });
    EXPECT_TRUE(timed_out.load());
}

TEST(TimedWait, CompletesWhenSignalledBeforeDeadline) {
    jaf::Scheduler sched(with_workers(2));
    std::atomic<bool> completed{false};
    sched.run([&] {
        jaf::Counter c{1};
        std::array<jaf::Task, 1> jobs = {jaf::Task{[&] { jaf::signal(c); }}};
        jaf::run(jobs, nullptr);
        const bool ok = jaf::wait_for_counter_until(&c, 0, std::chrono::steady_clock::now() + 5s);
        completed.store(ok);
    });
    EXPECT_TRUE(completed.load());
}

TEST(TimedWait, AlreadySatisfiedReturnsImmediately) {
    jaf::Scheduler sched(with_workers(2));
    std::atomic<bool> ok{false};
    sched.run([&] {
        jaf::Counter c{0};
        ok.store(jaf::wait_for_counter_until(&c, 0, std::chrono::steady_clock::now() + 5s));
    });
    EXPECT_TRUE(ok.load());
}

TEST(TryWait, ReflectsSatisfaction) {
    jaf::Scheduler sched(with_workers(2));
    sched.run([&] {
        jaf::Counter c{0};
        EXPECT_TRUE(jaf::try_wait_for_counter(&c, 0));

        jaf::Counter d{2};
        EXPECT_FALSE(jaf::try_wait_for_counter(&d, 0));
        jaf::signal(d, 2);
        EXPECT_TRUE(jaf::try_wait_for_counter(&d, 0));
    });
}
