// F-8: async / waitable graph submit -- jaf::schedule_async + jaf::GraphRun.

#include <jaf/Jaf.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>
#include <string>

namespace {

jaf::SchedulerOptions with_workers(unsigned n) {
    jaf::SchedulerOptions opt;
    opt.worker_count = n;
    return opt;
}

} // namespace

TEST(GraphAsync, TwoGraphsConcurrentJoinedIndependently) {
    jaf::Scheduler sched(with_workers(4));
    std::atomic<int> a{0}, b{0};
    sched.run([&] {
        jaf::Graph g1, g2;
        for (int i = 0; i < 8; ++i) g1.Emplace([&] { a.fetch_add(1, std::memory_order_relaxed); });
        for (int i = 0; i < 8; ++i) g2.Emplace([&] { b.fetch_add(1, std::memory_order_relaxed); });

        jaf::GraphRun r1 = jaf::schedule_async(g1);   // both in flight at once
        jaf::GraphRun r2 = jaf::schedule_async(g2);
        r1.wait();                                    // joined independently
        r2.wait();
    });
    EXPECT_EQ(a.load(), 8);
    EXPECT_EQ(b.load(), 8);
}

TEST(GraphAsync, ScheduleAndWaitFromMainThread) {
    // The whole point of F-8: the graph API is usable off a fiber. Here we submit
    // and join from the main thread (a plain, non-fiber thread).
    jaf::Scheduler sched(with_workers(4));
    std::atomic<int> n{0};
    jaf::Graph g;
    for (int i = 0; i < 16; ++i) g.Emplace([&] { n.fetch_add(1, std::memory_order_relaxed); });

    jaf::GraphRun r = jaf::schedule_async(g);
    r.wait();   // blocking wait (F-10) -- no worker/fiber involved on this thread
    EXPECT_EQ(n.load(), 16);
}

TEST(GraphAsync, ThrowingNodeSurfacesAtItsOwnWait) {
    jaf::Scheduler sched(with_workers(4));
    std::string msg;
    sched.run([&] {
        jaf::Graph g;
        g.Emplace([] { throw std::runtime_error("boom"); });
        jaf::GraphRun r = jaf::schedule_async(g);
        try {
            r.wait();
        } catch (const std::runtime_error& e) {
            msg = e.what();
        }
    });
    EXPECT_EQ(msg, "boom");
}

TEST(GraphAsync, DoneReflectsCompletion) {
    jaf::Scheduler sched(with_workers(2));
    sched.run([&] {
        jaf::Counter gate{1};
        jaf::Graph   g;
        g.Emplace([&] { jaf::wait_for_counter(&gate, 0); });   // node parks until released

        jaf::GraphRun r = jaf::schedule_async(g);
        EXPECT_FALSE(r.done());   // node still parked on gate
        jaf::signal(gate);        // release it
        r.wait();
        EXPECT_TRUE(r.done());    // consumed handle reports done
    });
}

TEST(GraphAsyncDeathTest, DestroyWithoutWaitAborts) {
    EXPECT_DEATH(
        {
            jaf::Scheduler sched(with_workers(2));
            jaf::Graph     g;
            g.Emplace([] {});
            jaf::GraphRun r = jaf::schedule_async(g);
            (void)r;   // dropped without a completed wait() -> hard abort
        },
        "without a completed wait");
}
