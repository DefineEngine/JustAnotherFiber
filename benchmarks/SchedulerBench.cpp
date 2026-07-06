// =============================================================================
//  benchmarks/scheduler_bench.cpp -- L2 scheduler micro-benchmarks.
//
//  Throughput (jobs/sec), single-job round-trip latency (submit + schedule +
//  run + wake), and scaling vs worker count. Each measurement loop runs INSIDE a
//  root job, because jaf::wait_for_counter is only legal on a fiber.
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

// One "operation" = submit `batch` trivial jobs against a counter and join. The
// timed loop runs on the root fiber so wait_for_counter is legal.
double bench_batch(jaf::Scheduler& sched, char const* name, std::uint64_t iters, int batch) {
    double ns_per_op = 0.0;
    sched.run([&] {
        ns_per_op = jaf::bench::run(name, iters, [&] {
            std::atomic<int> sum{0};
            jaf::Counter    c;
            std::vector<jaf::Task> jobs;
            jobs.reserve(static_cast<std::size_t>(batch));
            for (int i = 0; i < batch; ++i) {
                jobs.emplace_back([&sum] { sum.fetch_add(1, std::memory_order_relaxed); });
            }
            jaf::run(jobs, &c);
            jaf::wait_for_counter(&c, 0);
            jaf::bench::keep(sum.load());
        });
    });
    return ns_per_op;
}

} // namespace

int main() {
    unsigned const hw = std::max(1u, std::thread::hardware_concurrency());
    std::printf("hardware_concurrency = %u\n\n", hw);

    constexpr int kBatch = 512;

    // 1. Throughput / amortized per-job overhead at full width.
    {
        jaf::SchedulerOptions opt;
        opt.worker_count = hw;
        jaf::Scheduler sched(opt);
        double const ns = bench_batch(sched, "throughput: 512-job batch + join", 2000, kBatch);
        std::printf("  => %8.1f ns/job   %7.2f M jobs/sec\n\n",
                    ns / kBatch, 1000.0 / (ns / kBatch));
    }

    // 2. Single-job round-trip: submit + schedule + run + wake latency.
    {
        jaf::SchedulerOptions opt;
        opt.worker_count = hw;
        jaf::Scheduler sched(opt);
        bench_batch(sched, "single-job round-trip", 50000, 1);
        std::printf("\n");
    }

    // 3. Scaling vs worker count.
    std::printf("scaling (512-job batch + join):\n");
    for (unsigned w = 1; w <= hw; w *= 2) {
        jaf::SchedulerOptions opt;
        opt.worker_count = w;
        jaf::Scheduler sched(opt);
        double const ns = bench_batch(sched, "  (batch)", 1000, kBatch);
        std::printf("    workers=%-3u  %8.1f ns/job  %7.2f M jobs/sec\n",
                    w, ns / kBatch, 1000.0 / (ns / kBatch));
    }

    return 0;
}
