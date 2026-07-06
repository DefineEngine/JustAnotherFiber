// =============================================================================
//  benchmarks/graph_bench.cpp -- L3 DAG (jaf::Graph) micro-benchmarks.
//
//  Measures the per-node overhead of the graph layer in three shapes:
//    1. fan-out/fan-in DAG, rebuilt + scheduled each iteration;
//    2. the same DAG built once and re-scheduled (isolates run cost from build);
//    3. a deep linear chain (worst case: every node waits on the previous);
//  and a flat jaf::run batch of the same node count as the baseline the graph
//  layer is lowered onto. Each loop runs INSIDE a root job because jaf::schedule
//  parks the calling fiber.
// =============================================================================

#include <jaf/Jaf.hpp>

#include "Bench.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

// Build + schedule a fan-out/fan-in DAG (source -> width middles -> sink) each
// iteration. One "operation" = one full graph run.
double bench_fanout(jaf::Scheduler& sched, char const* name, std::uint64_t iters, int width) {
    double ns = 0.0;
    sched.run([&] {
        ns = jaf::bench::run(name, iters, [&] {
            std::atomic<int> sum{0};
            jaf::Graph       g;
            auto             src  = g.Emplace([] {});
            auto             sink = g.Emplace([] {});
            for (int i = 0; i < width; ++i) {
                auto mid = g.Emplace([&] { sum.fetch_add(1, std::memory_order_relaxed); });
                src.Precede(mid);
                mid.Precede(sink);
            }
            jaf::schedule(g);
            jaf::bench::keep(sum.load());
        });
    });
    return ns;
}

// Build the DAG once, re-schedule it each iteration -- isolates scheduling cost
// from graph construction/allocation cost.
double bench_reuse(jaf::Scheduler& sched, char const* name, std::uint64_t iters, int width) {
    double ns = 0.0;
    sched.run([&] {
        std::atomic<int> sum{0};
        jaf::Graph       g;
        auto             src  = g.Emplace([] {});
        auto             sink = g.Emplace([] {});
        for (int i = 0; i < width; ++i) {
            auto mid = g.Emplace([&] { sum.fetch_add(1, std::memory_order_relaxed); });
            src.Precede(mid);
            mid.Precede(sink);
        }
        ns = jaf::bench::run(name, iters, [&] {
            jaf::schedule(g);
            jaf::bench::keep(sum.load());
        });
    });
    return ns;
}

// Deep linear chain of `depth` nodes, rebuilt + scheduled each iteration.
double bench_chain(jaf::Scheduler& sched, char const* name, std::uint64_t iters, int depth) {
    double ns = 0.0;
    sched.run([&] {
        ns = jaf::bench::run(name, iters, [&] {
            std::atomic<int> sum{0};
            jaf::Graph       g;
            jaf::Graph::Node prev;
            for (int i = 0; i < depth; ++i) {
                auto n = g.Emplace([&] { sum.fetch_add(1, std::memory_order_relaxed); });
                if (prev) prev.Precede(n);
                prev = n;
            }
            jaf::schedule(g);
            jaf::bench::keep(sum.load());
        });
    });
    return ns;
}

// Baseline: the same node count via a flat jaf::run batch + join, no graph.
double bench_flat(jaf::Scheduler& sched, char const* name, std::uint64_t iters, int count) {
    double ns = 0.0;
    sched.run([&] {
        ns = jaf::bench::run(name, iters, [&] {
            std::atomic<int>       sum{0};
            jaf::Counter           c;
            std::vector<jaf::Task> jobs;
            jobs.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                jobs.emplace_back([&] { sum.fetch_add(1, std::memory_order_relaxed); });
            }
            jaf::run(jobs, &c);
            jaf::wait_for_counter(&c, 0);
            jaf::bench::keep(sum.load());
        });
    });
    return ns;
}

} // namespace

int main() {
    unsigned const hw = std::max(1u, std::thread::hardware_concurrency());
    std::printf("hardware_concurrency = %u\n\n", hw);

    constexpr int kWidth = 256;
    constexpr int kNodes = kWidth + 2;   // source + middles + sink

    jaf::SchedulerOptions opt;
    opt.worker_count = hw;
    jaf::Scheduler sched(opt);

    std::printf("DAG: source -> %d middles -> sink  (%d nodes)\n", kWidth, kNodes);
    {
        double const ns = bench_fanout(sched, "build + schedule (fan-out/in)", 2000, kWidth);
        std::printf("  => %8.1f ns/node\n", ns / kNodes);
    }
    {
        double const ns = bench_reuse(sched, "reschedule only (reuse graph)", 2000, kWidth);
        std::printf("  => %8.1f ns/node\n", ns / kNodes);
    }
    {
        double const ns = bench_flat(sched, "flat jaf::run batch (baseline)", 2000, kNodes);
        std::printf("  => %8.1f ns/node\n\n", ns / kNodes);
    }

    constexpr int kDepth = 256;
    std::printf("DAG: linear chain (%d nodes deep, fully sequential)\n", kDepth);
    {
        double const ns = bench_chain(sched, "build + schedule (chain)", 1000, kDepth);
        std::printf("  => %8.1f ns/node\n", ns / kDepth);
    }

    return 0;
}
