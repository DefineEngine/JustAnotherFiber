// F-4: fiber-aware synchronization primitives (jaf::Mutex/Event/ManualResetEvent/
// Semaphore) and the R-1 lock-held-across-wait debug detector.

#include <jaf/Jaf.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

jaf::SchedulerOptions with_workers(unsigned n) {
    jaf::SchedulerOptions opt;
    opt.worker_count = n;
    return opt;
}

} // namespace

TEST(Mutex, ExactCountUnderContention) {
    jaf::Scheduler sched(with_workers(4));
    jaf::Mutex m;
    int        shared = 0;   // plain int, only ever touched under m
    constexpr int N = 200;

    sched.run([&] {
        jaf::Counter c;
        std::vector<jaf::Task> jobs;
        jobs.reserve(N);
        for (int i = 0; i < N; ++i) {
            jobs.emplace_back([&] {
                m.lock();
                ++shared;     // fiber may have migrated across the (parking) lock
                m.unlock();
            });
        }
        jaf::run(jobs, &c);
        jaf::wait_for_counter(&c, 0);
    });

    EXPECT_EQ(shared, N);   // no lost updates
}

TEST(ManualResetEvent, SignalWakesAllWaiters) {
    jaf::Scheduler sched(with_workers(4));
    jaf::ManualResetEvent ev;
    std::atomic<int>      woke{0};
    constexpr int         W = 8;

    sched.run([&] {
        jaf::Counter c;
        std::vector<jaf::Task> waiters;
        waiters.reserve(W);
        for (int i = 0; i < W; ++i) {
            waiters.emplace_back([&] {
                ev.wait();
                woke.fetch_add(1, std::memory_order_relaxed);
            });
        }
        jaf::run(waiters, &c);
        ev.signal();               // stays set: waiters parked-or-not all proceed
        jaf::wait_for_counter(&c, 0);
    });

    EXPECT_EQ(woke.load(), W);
}

TEST(Event, AutoResetPendingSignalConsumedByNextWait) {
    jaf::Scheduler sched(with_workers(2));
    std::atomic<bool> proceeded{false};
    sched.run([&] {
        jaf::Event ev;
        ev.signal();   // no waiter -> one pending signal
        ev.wait();     // consumes it, returns without parking
        proceeded.store(true, std::memory_order_relaxed);
    });
    EXPECT_TRUE(proceeded.load());
}

TEST(Semaphore, LimitsConcurrency) {
    jaf::Scheduler sched(with_workers(4));
    jaf::Semaphore   sem(2);   // at most 2 fibers in the region at once
    std::atomic<int> current{0};
    std::atomic<int> max_seen{0};
    constexpr int    N = 20;

    sched.run([&] {
        jaf::Counter c;
        std::vector<jaf::Task> jobs;
        jobs.reserve(N);
        for (int i = 0; i < N; ++i) {
            jobs.emplace_back([&] {
                sem.acquire();
                const int cur = current.fetch_add(1, std::memory_order_acq_rel) + 1;
                int prev = max_seen.load(std::memory_order_relaxed);
                while (cur > prev &&
                       !max_seen.compare_exchange_weak(prev, cur, std::memory_order_relaxed)) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(1));  // overlap window
                current.fetch_sub(1, std::memory_order_acq_rel);
                sem.release();
            });
        }
        jaf::run(jobs, &c);
        jaf::wait_for_counter(&c, 0);
    });

    EXPECT_LE(max_seen.load(), 2);
}

#ifndef NDEBUG
TEST(MutexDeathTest, HoldingLockAcrossWaitAborts) {
    // R-1: holding a jaf::Mutex across a wait_for_counter is flagged in debug (the
    // fiber may resume on another thread). The assert fires before any park, so
    // the never-satisfied counter below never actually hangs.
    EXPECT_DEATH(
        {
            jaf::Scheduler sched(with_workers(2));
            sched.run([] {
                jaf::Mutex m;
                m.lock();
                jaf::Counter c{1};
                jaf::wait_for_counter(&c, 0);
            });
        },
        "holding a jaf::Mutex");
}
#endif
