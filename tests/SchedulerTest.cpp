// Tests for the L2 runtime: jaf::Scheduler, jaf::run, jaf::wait_for_counter.
//
// Determinism: single-worker runs fix ordering; multi-worker runs assert only on
// commutative outcomes (atomic sums / completion counts), never on interleaving.

#include <jaf/Jaf.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

jaf::SchedulerOptions with_workers(unsigned n) {
    jaf::SchedulerOptions opt;
    opt.worker_count = n;
    return opt;
}

// Build a batch of jobs that each increment `sum`.
std::vector<jaf::Task> make_increment_jobs(std::atomic<int>& sum, int count) {
    std::vector<jaf::Task> jobs;
    jobs.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        jobs.emplace_back([&sum] { sum.fetch_add(1, std::memory_order_relaxed); });
    }
    return jobs;
}

} // namespace

TEST(Scheduler, RunSingleRootJob) {
    jaf::Scheduler sched(with_workers(2));
    std::atomic<bool> ran{false};
    sched.run([&] { ran.store(true, std::memory_order_relaxed); });
    EXPECT_TRUE(ran.load());
}

TEST(Scheduler, RunNJobsJoinOnCounterSingleWorker) {
    jaf::Scheduler sched(with_workers(1));
    std::atomic<int> sum{0};
    constexpr int N = 200;
    sched.run([&] {
        jaf::Counter c;
        auto jobs = make_increment_jobs(sum, N);
        jaf::run(jobs, &c);
        jaf::wait_for_counter(&c, 0);
        // After the wait, every job in the batch has completed.
        EXPECT_EQ(sum.load(), N);
    });
    EXPECT_EQ(sum.load(), N);
}

TEST(Scheduler, RunNJobsJoinOnCounterMultiWorker) {
    jaf::Scheduler sched(with_workers(4));
    std::atomic<int> sum{0};
    constexpr int N = 1000;
    sched.run([&] {
        jaf::Counter c;
        auto jobs = make_increment_jobs(sum, N);
        jaf::run(jobs, &c);
        jaf::wait_for_counter(&c, 0);
    });
    EXPECT_EQ(sum.load(), N);
}

TEST(Scheduler, NestedRunAndWait) {
    jaf::Scheduler sched(with_workers(4));
    std::atomic<int> leaves{0};
    constexpr int kChildren = 8;
    constexpr int kGrand    = 8;

    sched.run([&] {
        jaf::Counter c;
        std::vector<jaf::Task> children;
        children.reserve(kChildren);
        for (int i = 0; i < kChildren; ++i) {
            children.emplace_back([&] {
                jaf::Counter cc;
                auto grand = make_increment_jobs(leaves, kGrand);
                jaf::run(grand, &cc);
                jaf::wait_for_counter(&cc, 0);
            });
        }
        jaf::run(children, &c);
        jaf::wait_for_counter(&c, 0);
    });

    EXPECT_EQ(leaves.load(), kChildren * kGrand);
}

TEST(Scheduler, FiberRecyclingHandlesManyJobs) {
    // Far more jobs than fibers: the looping trampoline must recycle stacks.
    jaf::SchedulerOptions opt = with_workers(4);
    opt.small_fibers = 16;
    opt.large_fibers = 4;
    jaf::Scheduler sched(opt);

    std::atomic<int> done{0};
    constexpr int N = 5000;
    sched.run([&] {
        jaf::Counter c;
        auto jobs = make_increment_jobs(done, N);
        jaf::run(jobs, &c);
        jaf::wait_for_counter(&c, 0);
    });
    EXPECT_EQ(done.load(), N);
}

TEST(Scheduler, WorkStealingSpreadsAcrossWorkers) {
    jaf::Scheduler sched(with_workers(4));
    std::atomic<int> sum{0};
    std::mutex m;
    std::set<std::thread::id> threads;
    constexpr int N = 40;

    sched.run([&] {
        jaf::Counter c;
        std::vector<jaf::Task> jobs;
        jobs.reserve(N);
        for (int i = 0; i < N; ++i) {
            jobs.emplace_back([&] {
                // A little dwell so one worker can't drain the whole batch before
                // siblings steal.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                sum.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lk(m);
                threads.insert(std::this_thread::get_id());
            });
        }
        jaf::run(jobs, &c);
        jaf::wait_for_counter(&c, 0);
    });

    EXPECT_EQ(sum.load(), N);
    EXPECT_GT(threads.size(), 1u);   // more than one worker ran jobs => stealing happened
}

TEST(Scheduler, ExceptionInRootRethrownFromRun) {
    jaf::Scheduler sched(with_workers(2));
    bool threw = false;
    try {
        sched.run([] { throw std::runtime_error("boom"); });
    } catch (const std::runtime_error& e) {
        threw = true;
        EXPECT_STREQ(e.what(), "boom");
    }
    EXPECT_TRUE(threw);
}

TEST(Scheduler, ExceptionInChildJobSurfacesAndGraphStillDrains) {
    jaf::Scheduler sched(with_workers(4));
    std::atomic<int> ran{0};
    bool threw = false;
    try {
        sched.run([&] {
            jaf::Counter c;
            std::vector<jaf::Task> jobs;
            jobs.emplace_back([&] {
                ran.fetch_add(1, std::memory_order_relaxed);
                throw std::runtime_error("child boom");
            });
            jobs.emplace_back([&] { ran.fetch_add(1, std::memory_order_relaxed); });
            jaf::run(jobs, &c);
            jaf::wait_for_counter(&c, 0);   // both still complete (exception is captured)
        });
    } catch (const std::exception&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(ran.load(), 2);
}

TEST(Scheduler, WaitCheckedObservesJobFailureAtJoin) {
    // C-4: a throwing job poisons its counter; wait_for_counter_checked reports
    // the failure AT THE JOIN (returns false), not only at Scheduler::run.
    jaf::Scheduler sched(with_workers(4));
    std::atomic<bool> failure_seen_at_join{false};
    std::atomic<int>  ran{0};
    bool              run_rethrew = false;

    try {
        sched.run([&] {
            jaf::Counter c;
            std::vector<jaf::Task> jobs;
            jobs.emplace_back([&] {
                ran.fetch_add(1, std::memory_order_relaxed);
                throw std::runtime_error("job boom");
            });
            jobs.emplace_back([&] { ran.fetch_add(1, std::memory_order_relaxed); });
            jaf::run(jobs, &c);
            const bool ok = jaf::wait_for_counter_checked(&c, 0);
            failure_seen_at_join.store(!ok, std::memory_order_relaxed);
        });
    } catch (const std::exception&) {
        run_rethrew = true;
    }

    EXPECT_TRUE(failure_seen_at_join.load());  // observed via the checked wait
    EXPECT_EQ(ran.load(), 2);                  // both jobs still decremented the counter
    EXPECT_TRUE(run_rethrew);                  // and the exception still surfaces globally
}

TEST(Scheduler, WaitCheckedReturnsTrueWhenAllSucceed) {
    jaf::Scheduler sched(with_workers(4));
    std::atomic<int>  sum{0};
    std::atomic<bool> ok_seen{false};
    sched.run([&] {
        jaf::Counter c;
        auto jobs = make_increment_jobs(sum, 4);
        jaf::run(jobs, &c);
        ok_seen.store(jaf::wait_for_counter_checked(&c, 0), std::memory_order_relaxed);
    });
    EXPECT_TRUE(ok_seen.load());
    EXPECT_EQ(sum.load(), 4);
}

TEST(WatchdogDeathTest, NeverSatisfiedCounterAborts) {
    // R-2: with the opt-in watchdog on, a fiber parked on a counter that is never
    // signalled (and no I/O outstanding) is detected as a deadlock and aborts,
    // instead of hanging. Short window so it fails fast.
    EXPECT_DEATH(
        {
            jaf::SchedulerOptions opt;
            opt.worker_count            = 2;
            opt.enable_watchdog         = true;
            opt.pool_exhaustion_timeout = std::chrono::milliseconds(25);
            jaf::Scheduler sched(opt);
            sched.run([] {
                jaf::Counter c{1};              // never signalled
                jaf::wait_for_counter(&c, 0);   // parks forever -> watchdog fires
            });
        },
        "deadlock");
}

TEST(Stats, ReportsPoolHeadroomAtInit) {
    // R-2: stats() exposes pool occupancy so an engine can assert headroom.
    jaf::SchedulerOptions opt;
    opt.worker_count = 2;
    opt.small_fibers = 8;
    opt.large_fibers = 4;
    jaf::Scheduler sched(opt);

    const jaf::Scheduler::Stats s = sched.stats();
    EXPECT_EQ(s.small_total, 8u);
    EXPECT_EQ(s.large_total, 4u);
    EXPECT_EQ(s.small_in_use, 0u);   // nothing running/parked before run()
    EXPECT_EQ(s.large_in_use, 0u);
}

TEST(Stats, CountsJobsRunAcrossWorkers) {
    // R-3: per-worker job counters aggregate in stats(). Also exercises the R-3
    // `name` parameter on jaf::run (profiler zone label).
    jaf::Scheduler sched(with_workers(2));
    std::atomic<int> sum{0};
    sched.run([&] {
        jaf::Counter c;
        auto jobs = make_increment_jobs(sum, 10);
        jaf::run(jobs, &c, jaf::StackClass::Small, jaf::Priority::Normal, "test-jobs");
        jaf::wait_for_counter(&c, 0);
    });
    const jaf::Scheduler::Stats s = sched.stats();
    EXPECT_EQ(sum.load(), 10);
    EXPECT_GE(s.jobs_run, 10u);   // 10 jobs + the root, spread across workers
}

TEST(Affinity, PinnedJobStaysOnPumpThreadAcrossWait) {
    // F-5: a job submitted via run_on(Main) runs on the thread that pumps Main and
    // -- crucially -- resumes on that SAME thread after a wait_for_counter, even
    // though a background worker signalled the counter.
    jaf::Scheduler sched(with_workers(2));

    std::thread::id   pinned_tid_before, pinned_tid_after, pump_tid;
    std::atomic<bool> done{false};
    std::atomic<bool> stop_pump{false};

    std::thread pump_thread([&] {
        pump_tid = std::this_thread::get_id();
        while (!stop_pump.load(std::memory_order_acquire)) {
            sched.pump(jaf::ThreadTag::Main);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        sched.pump(jaf::ThreadTag::Main);   // final drain after stop
    });

    sched.run([&] {
        jaf::Counter gate{1};          // signalled by a background job
        jaf::Counter pinned_done;

        std::vector<jaf::Task> pinned;
        pinned.emplace_back([&] {
            pinned_tid_before = std::this_thread::get_id();
            jaf::wait_for_counter(&gate, 0);            // parks; resumes via pump()
            pinned_tid_after = std::this_thread::get_id();
            done.store(true, std::memory_order_relaxed);
        });
        jaf::run_on(jaf::ThreadTag::Main, pinned, &pinned_done);

        jaf::Counter bg;
        std::vector<jaf::Task> bgjobs;
        bgjobs.emplace_back([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            jaf::signal(gate);
        });
        jaf::run(bgjobs, &bg);

        jaf::wait_for_counter(&pinned_done, 0);
        jaf::wait_for_counter(&bg, 0);
    });

    stop_pump.store(true, std::memory_order_release);
    pump_thread.join();

    EXPECT_TRUE(done.load());
    EXPECT_EQ(pinned_tid_before, pinned_tid_after);   // same thread across the wait
    EXPECT_EQ(pinned_tid_before, pump_tid);           // ...the pump thread
}

TEST(MainThreadWorker, CallerParticipatesToRunConcurrentWork) {
    // F-6: one background worker + the caller lane = two execution contexts. Two
    // jobs each busy-spin until BOTH have started, so they can only complete if
    // they run concurrently -- which requires the caller thread to run one of them
    // while the single background worker runs the other. (Without the caller lane
    // this would hang; CTest's TIMEOUT is the backstop.)
    jaf::SchedulerOptions opt;
    opt.worker_count     = 1;
    opt.caller_is_worker = true;
    jaf::Scheduler sched(opt);

    std::atomic<int>  started{0};
    std::atomic<bool> both_ran{false};
    sched.run([&] {
        jaf::Counter c;
        std::vector<jaf::Task> jobs;
        for (int i = 0; i < 2; ++i) {
            jobs.emplace_back([&] {
                started.fetch_add(1, std::memory_order_acq_rel);
                while (started.load(std::memory_order_acquire) < 2) { /* need 2 lanes */ }
                both_ran.store(true, std::memory_order_relaxed);
            });
        }
        jaf::run(jobs, &c);
        jaf::wait_for_counter(&c, 0);
    });

    EXPECT_TRUE(both_ran.load());
}

TEST(Priority, HighJumpsLowBacklogSingleWorker) {
    // F-3: on one worker, the root (running on that worker) queues a Low burst and
    // then a single High job before parking. When the worker frees up it scans
    // High first, so the High job records completion order 0 -- ahead of every Low.
    // caller_is_worker=false keeps this to exactly ONE execution context (F-6 would
    // otherwise add the caller lane and break the strict ordering assumption).
    jaf::SchedulerOptions opt;
    opt.worker_count     = 1;
    opt.caller_is_worker = false;
    jaf::Scheduler sched(opt);
    std::atomic<int> order{0};
    std::atomic<int> high_order{-1};
    constexpr int    N = 8;

    sched.run([&] {
        jaf::Counter c;
        std::vector<jaf::Task> lows;
        lows.reserve(N);
        for (int i = 0; i < N; ++i) {
            lows.emplace_back([&] { order.fetch_add(1, std::memory_order_relaxed); });
        }
        jaf::run(lows, &c, jaf::StackClass::Small, jaf::Priority::Low);

        std::vector<jaf::Task> highs;
        highs.emplace_back([&] {
            high_order.store(order.fetch_add(1, std::memory_order_relaxed),
                             std::memory_order_relaxed);
        });
        jaf::run(highs, &c, jaf::StackClass::Small, jaf::Priority::High);

        jaf::wait_for_counter(&c, 0);
    });

    EXPECT_EQ(high_order.load(), 0);      // High ran before any Low drained
    EXPECT_EQ(order.load(), N + 1);
}

TEST(Signal, ExternalThreadWakesParkedFiber) {
    // F-1: a plain std::thread completes an event counter; the parked fiber wakes
    // through the normal counter machinery.
    jaf::Scheduler sched(with_workers(2));
    std::atomic<bool> woke{false};
    sched.run([&] {
        jaf::Counter c{1};                       // event counter: starts unsignalled
        std::thread signaller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            jaf::signal(c);                      // external completion from off-worker
        });
        jaf::wait_for_counter(&c, 0);            // parks until signalled
        woke.store(true, std::memory_order_relaxed);
        signaller.join();
    });
    EXPECT_TRUE(woke.load());
}

TEST(SchedulerDeathTest, WaitForCounterOutsideFiberAborts) {
    // Calling wait_for_counter on the main thread (no current fiber) is a hard,
    // always-on misuse check.
    jaf::Counter c{1};
    EXPECT_DEATH({ jaf::wait_for_counter(&c, 0); }, "inside a fiber");
}
