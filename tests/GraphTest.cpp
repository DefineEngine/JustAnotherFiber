// Tests for the L3 DAG API: jaf::Graph, Graph::Node, jaf::schedule.
//
// Every schedule() runs inside a root job (it parks the calling fiber, like
// wait_for_counter). Determinism: single-worker runs fix ordering; multi-worker
// runs assert only on commutative outcomes (sums / "ran after both" flags).

#include <jaf/Jaf.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {

jaf::SchedulerOptions with_workers(unsigned n) {
    jaf::SchedulerOptions opt;
    opt.worker_count = n;
    return opt;
}

} // namespace

TEST(Graph, EmptyGraphScheduleNoOps) {
    jaf::Scheduler sched(with_workers(2));
    sched.run([] {
        jaf::Graph g;
        EXPECT_EQ(g.Size(), 0u);
        jaf::schedule(g);   // no nodes: returns immediately
    });
    SUCCEED();
}

TEST(Graph, SingleNodeRuns) {
    jaf::Scheduler sched(with_workers(2));
    std::atomic<bool> ran{false};
    sched.run([&] {
        jaf::Graph g;
        g.Emplace([&] { ran.store(true, std::memory_order_relaxed); });
        EXPECT_EQ(g.Size(), 1u);
        jaf::schedule(g);
    });
    EXPECT_TRUE(ran.load());
}

TEST(Graph, LinearChainRunsInOrder) {
    // Single worker => the chain executes strictly sequentially.
    jaf::Scheduler sched(with_workers(1));
    std::vector<int> order;
    std::mutex       m;
    auto record = [&](int id) {
        std::lock_guard<std::mutex> lk(m);
        order.push_back(id);
    };

    sched.run([&] {
        jaf::Graph g;
        auto a = g.Emplace([&] { record(1); });
        auto b = g.Emplace([&] { record(2); });
        auto c = g.Emplace([&] { record(3); });
        a.Precede(b);
        b.Precede(c);
        jaf::schedule(g);
    });

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
}

TEST(Graph, DiamondRunsAfterBothBranches) {
    // A -> {B, C} -> D. D must observe both B and C complete before it runs.
    jaf::Scheduler sched(with_workers(4));
    std::atomic<bool> a_ran{false}, b_ran{false}, c_ran{false};
    std::atomic<bool> d_saw_both{false};

    sched.run([&] {
        jaf::Graph g;
        auto a = g.Emplace([&] { a_ran.store(true, std::memory_order_relaxed); });
        auto b = g.Emplace([&] { b_ran.store(true, std::memory_order_release); });
        auto c = g.Emplace([&] { c_ran.store(true, std::memory_order_release); });
        auto d = g.Emplace([&] {
            const bool both = b_ran.load(std::memory_order_acquire) &&
                              c_ran.load(std::memory_order_acquire);
            d_saw_both.store(both, std::memory_order_relaxed);
        });
        a.Precede(b, c);   // variadic: a before both b and c
        d.Succeed(b, c);   // d after both b and c
        jaf::schedule(g);
    });

    EXPECT_TRUE(a_ran.load());
    EXPECT_TRUE(b_ran.load());
    EXPECT_TRUE(c_ran.load());
    EXPECT_TRUE(d_saw_both.load());
}

TEST(Graph, WideFanOutFanInJoins) {
    jaf::Scheduler sched(with_workers(4));
    std::atomic<int>  sum{0};
    std::atomic<bool> sink_ran{false};
    constexpr int     N = 256;

    sched.run([&] {
        jaf::Graph g;
        auto src  = g.Emplace([] {});
        auto sink = g.Emplace([&] { sink_ran.store(true, std::memory_order_relaxed); });
        for (int i = 0; i < N; ++i) {
            auto mid = g.Emplace([&] { sum.fetch_add(1, std::memory_order_relaxed); });
            src.Precede(mid);
            mid.Precede(sink);
        }
        EXPECT_EQ(g.Size(), static_cast<std::size_t>(N + 2));
        jaf::schedule(g);
        // After schedule returns, the whole DAG (including the sink) has drained.
        EXPECT_EQ(sum.load(), N);
        EXPECT_TRUE(sink_ran.load());
    });

    EXPECT_EQ(sum.load(), N);
}

TEST(Graph, FluentChainingAndStackClass) {
    // Succeed() + chained setters + a large-stack node.
    jaf::Scheduler sched(with_workers(2));
    std::atomic<bool> deep_ran{false}, after_ran{false};

    sched.run([&] {
        jaf::Graph g;
        auto deep = g.Emplace([&] {
            deep_ran.store(true, std::memory_order_release);
        }).Stack(jaf::StackClass::Large);
        g.Emplace([&] {
            after_ran.store(true, std::memory_order_relaxed);
        }).Succeed(deep);
        jaf::schedule(g);
    });

    EXPECT_TRUE(deep_ran.load());
    EXPECT_TRUE(after_ran.load());
}

TEST(Graph, PrunesDownstreamConeOnException) {
    // Chain a -> b -> c with b throwing, plus an independent node d. b's failure
    // prunes c (it never runs); d, on an independent branch, still runs; the
    // exception surfaces from Scheduler::run.
    jaf::Scheduler sched(with_workers(4));
    std::atomic<bool> a_ran{false}, c_ran{false}, d_ran{false};
    bool              threw = false;

    try {
        sched.run([&] {
            jaf::Graph g;
            auto a = g.Emplace([&] { a_ran.store(true, std::memory_order_relaxed); });
            auto b = g.Emplace([] { throw std::runtime_error("node boom"); });
            auto c = g.Emplace([&] { c_ran.store(true, std::memory_order_relaxed); });
            g.Emplace([&] { d_ran.store(true, std::memory_order_relaxed); });   // independent
            a.Precede(b);
            b.Precede(c);
            jaf::schedule(g);
        });
    } catch (const std::runtime_error& e) {
        threw = true;
        EXPECT_STREQ(e.what(), "node boom");
    }

    EXPECT_TRUE(threw);
    EXPECT_TRUE(a_ran.load());
    EXPECT_FALSE(c_ran.load());   // pruned: predecessor b threw
    EXPECT_TRUE(d_ran.load());    // independent branch unaffected
}

TEST(Graph, ReuseSchedulesTwice) {
    // ResetRunState must re-arm the in-degrees so the same graph runs again.
    jaf::Scheduler sched(with_workers(4));
    std::atomic<int> sum{0};

    sched.run([&] {
        jaf::Graph g;
        auto a = g.Emplace([&] { sum.fetch_add(1, std::memory_order_relaxed); });
        auto b = g.Emplace([&] { sum.fetch_add(1, std::memory_order_relaxed); });
        auto c = g.Emplace([&] { sum.fetch_add(1, std::memory_order_relaxed); });
        a.Precede(b, c);

        jaf::schedule(g);
        EXPECT_EQ(sum.load(), 3);

        jaf::schedule(g);
        EXPECT_EQ(sum.load(), 6);
    });

    EXPECT_EQ(sum.load(), 6);
}

TEST(Graph, NodePriorityAndStackFluent) {
    // F-3: Node::Prio composes with Stack() and the graph runs to completion.
    jaf::Scheduler sched(with_workers(2));
    std::atomic<int> sum{0};
    sched.run([&] {
        jaf::Graph g;
        g.Emplace([&] { sum.fetch_add(1, std::memory_order_relaxed); })
            .Prio(jaf::Priority::High);
        g.Emplace([&] { sum.fetch_add(1, std::memory_order_relaxed); })
            .Prio(jaf::Priority::Low)
            .Stack(jaf::StackClass::Large);
        jaf::schedule(g);
    });
    EXPECT_EQ(sum.load(), 2);
}

TEST(Graph, ConcurrentGraphsSurfaceTheirOwnException) {
    // C-4: two graphs scheduled concurrently, each with a throwing node. Each
    // schedule() must rethrow ITS OWN graph's exception -- proving graph errors
    // are attributed per-run, not funnelled through one scheduler-global slot.
    jaf::Scheduler sched(with_workers(4));
    std::atomic<int> caught_a{0}, caught_b{0}, wrong{0};

    sched.run([&] {
        jaf::Counter c;
        std::vector<jaf::Task> jobs;
        jobs.emplace_back([&] {
            jaf::Graph g;
            g.Emplace([] { throw std::runtime_error("graph-A"); });
            try {
                jaf::schedule(g);
            } catch (const std::runtime_error& e) {
                (std::strcmp(e.what(), "graph-A") == 0 ? caught_a : wrong)
                    .fetch_add(1, std::memory_order_relaxed);
            }
        });
        jobs.emplace_back([&] {
            jaf::Graph g;
            g.Emplace([] { throw std::runtime_error("graph-B"); });
            try {
                jaf::schedule(g);
            } catch (const std::runtime_error& e) {
                (std::strcmp(e.what(), "graph-B") == 0 ? caught_b : wrong)
                    .fetch_add(1, std::memory_order_relaxed);
            }
        });
        jaf::run(jobs, &c);
        jaf::wait_for_counter(&c, 0);
    });

    EXPECT_EQ(caught_a.load(), 1);
    EXPECT_EQ(caught_b.load(), 1);
    EXPECT_EQ(wrong.load(), 0);   // never crossed
}

TEST(Graph, ComposeRunsSubgraphAsFencedRegion) {
    // F-11: a composed subgraph runs entirely between the parent's before/after
    // nodes, ordered via the returned {entry, exit} handles. Ordering is enforced
    // by DAG edges (deterministic on any worker count).
    jaf::Scheduler sched(with_workers(4));
    std::atomic<int> order{0};
    std::atomic<int> before_val{-1}, sub_val{-1}, after_val{-1};

    sched.run([&] {
        jaf::Graph sub;
        sub.Emplace([&] { sub_val.store(order.fetch_add(1), std::memory_order_relaxed); });

        jaf::Graph g;
        auto before = g.Emplace([&] { before_val.store(order.fetch_add(1), std::memory_order_relaxed); });
        auto [entry, exit] = g.Compose(sub);      // sub is consumed
        auto after = g.Emplace([&] { after_val.store(order.fetch_add(1), std::memory_order_relaxed); });
        before.Precede(entry);
        exit.Precede(after);

        jaf::schedule(g);
    });

    EXPECT_GE(before_val.load(), 0);
    EXPECT_LT(before_val.load(), sub_val.load());   // sub after before
    EXPECT_LT(sub_val.load(), after_val.load());    // after after sub
}

TEST(GraphDeathTest, CyclicGraphAborts) {
    // C-3: cycle detection is always-on (this fires in Release too, not just
    // Debug), so a cyclic graph never silently executes a partial frame.
    EXPECT_DEATH(
        {
            jaf::Scheduler sched(with_workers(2));
            sched.run([] {
                jaf::Graph g;
                auto a = g.Emplace([] {});
                auto b = g.Emplace([] {});
                a.Precede(b);
                b.Precede(a);   // cycle: neither ever reaches in-degree 0
                jaf::schedule(g);
            });
        },
        "cycle");
}

TEST(GraphDeathTest, ScheduleOutsideFiberAborts) {
    // schedule() parks the caller; calling it off a fiber (here, the main thread)
    // is a hard, always-on misuse check.
    jaf::Graph g;
    g.Emplace([] {});
    EXPECT_DEATH({ jaf::schedule(g); }, "inside a fiber");
}
