// F-10: blocking wait for NON-fiber threads -- wait_for_counter_blocking[_until].
// These let an editor tool thread / the main thread join jaf work (a fiber must
// use wait_for_counter instead).

#include <jaf/Jaf.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace {

jaf::SchedulerOptions with_workers(unsigned n) {
    jaf::SchedulerOptions opt;
    opt.worker_count = n;
    return opt;
}

using namespace std::chrono_literals;

} // namespace

TEST(BlockingWait, WakesOnSignalFromAnotherThread) {
    jaf::Scheduler sched(with_workers(2));
    jaf::Counter   c{1};
    std::thread    t([&] {
        std::this_thread::sleep_for(30ms);
        jaf::signal(c);
    });
    const bool ok = jaf::wait_for_counter_blocking(&c, 0);   // main thread blocks
    t.join();
    EXPECT_TRUE(ok);
}

TEST(BlockingWait, AlreadySatisfiedReturnsImmediately) {
    jaf::Scheduler sched(with_workers(2));
    jaf::Counter   c{0};
    EXPECT_TRUE(jaf::wait_for_counter_blocking(&c, 0));
}

TEST(BlockingWait, TimedBlockingTimesOut) {
    jaf::Scheduler sched(with_workers(2));
    jaf::Counter   c{1};   // never signalled
    const bool ok =
        jaf::wait_for_counter_blocking_until(&c, 0, std::chrono::steady_clock::now() + 40ms);
    EXPECT_FALSE(ok);
}

TEST(BlockingWait, TimedBlockingCompletesWhenSignalled) {
    jaf::Scheduler sched(with_workers(2));
    jaf::Counter   c{1};
    std::thread    t([&] {
        std::this_thread::sleep_for(20ms);
        jaf::signal(c);
    });
    const bool ok =
        jaf::wait_for_counter_blocking_until(&c, 0, std::chrono::steady_clock::now() + 5s);
    t.join();
    EXPECT_TRUE(ok);
}
