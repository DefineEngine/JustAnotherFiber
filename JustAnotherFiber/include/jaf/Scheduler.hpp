#pragma once

// jaf/Scheduler.hpp -- worker-thread pool that runs jobs on recyclable fibers,
// with counter-based synchronization.

#include <jaf/Config.hpp>
#include <jaf/Counter.hpp>
#include <jaf/Cancellation.hpp>
#include <jaf/Task.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace jaf {

/// Specifies the stack size class for a fiber.
enum class StackClass : std::uint8_t {
    Small,   ///< Default fiber stack size (config::kDefaultStackSize).
    Large,   ///< Large fiber stack size (config::kLargeStackSize).
};

/// Scheduling priority; workers scan queues High -> Normal -> Low. The numeric
/// values ARE the queue indices; do not reorder.
enum class Priority : std::uint8_t {
    High   = 0,
    Normal = 1,
    Low    = 2,
};

/// A named engine thread that runs pinned jobs. Numeric values index the
/// named-worker array; keep Count last.
enum class ThreadTag : std::uint8_t {
    Main   = 0,
    Render = 1,
    Count,   ///< sentinel: number of tags
};

/// Options for configuring the task scheduler.
struct SchedulerOptions {
    std::size_t worker_count = 0;     ///< Number of BACKGROUND worker threads. 0 defaults to hardware concurrency.
    bool        pin_threads  = false; ///< Pin worker threads to dedicated logical cores.
    std::size_t small_fibers = config::kDefaultSmallFibers; ///< Pre-allocated small fibers count.
    std::size_t large_fibers = config::kDefaultLargeFibers; ///< Pre-allocated large fibers count.
    /// When true (default), the thread that calls Scheduler::run participates as a worker while waiting.
    bool        caller_is_worker = true;
    /// How long every worker must be stalled on pool exhaustion before the watchdog declares a livelock.
    std::chrono::milliseconds pool_exhaustion_timeout = config::kPoolExhaustionTimeout;
    /// Opt-in deadlock watchdog; leave OFF if you use external signalling (GPU fence / audio / your threads).
    bool enable_watchdog = false;
};

namespace detail {
struct SchedulerImpl;
[[nodiscard]] unsigned ActiveWorkerCount() noexcept;
} // namespace detail

/// The task scheduler running tasks on a worker thread pool.
class Scheduler {
public:
    /// Construct the scheduler and start its worker pool.
    explicit Scheduler(SchedulerOptions options = {});
    ~Scheduler();

    Scheduler(const Scheduler&)            = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&)                 = delete;
    Scheduler& operator=(Scheduler&&)      = delete;

    /// Submit a root task and block until the entire task graph completes; rethrows the first job exception.
    void run(Task root);

    /// Run any jobs pinned to `tag` (via run_on) that are currently ready, on the CALLING thread.
    void pump(ThreadTag tag);

    /// A runtime occupancy snapshot; values are sampled without a global lock and are approximate.
    struct Stats {
        std::size_t   small_total   = 0;
        std::size_t   small_in_use  = 0;   ///< Small fibers running or parked.
        std::size_t   large_total   = 0;
        std::size_t   large_in_use  = 0;
        std::int64_t  inflight      = 0;   ///< Jobs submitted but not yet complete.
        std::int64_t  pending       = 0;   ///< Runnable items queued across workers.
        std::uint64_t jobs_run      = 0;   ///< Total jobs completed (all workers).
        std::uint64_t steals        = 0;   ///< Total successful steals (all workers).
    };
    [[nodiscard]] Stats stats() const noexcept;

private:
    std::unique_ptr<detail::SchedulerImpl> m_impl;
};

/// Submit a batch of jobs; if `counter` is non-null it is bumped by jobs.size() and each job decrements it.
void run(std::span<Task> jobs, Counter* counter, StackClass stack_class = StackClass::Small,
         Priority prio = Priority::Normal, const char* name = nullptr);

/// As run(), but each job observes `token`; a cancelled job is skipped yet still decrements `counter`.
void run(std::span<Task> jobs, Counter* counter, const CancellationToken& token,
         StackClass stack_class = StackClass::Small, Priority prio = Priority::Normal);

/// Submit jobs pinned to a named thread; they run only when that thread calls Scheduler::pump(tag).
void run_on(ThreadTag tag, std::span<Task> jobs, Counter* counter,
            StackClass stack_class = StackClass::Small, Priority prio = Priority::Normal,
            const char* name = nullptr);

/// Park the current fiber until `counter` reaches `target`; MUST be called from inside a fiber/job.
void wait_for_counter(Counter* counter, std::int64_t target = 0);

/// Like wait_for_counter, but returns false if any job that decremented this counter threw.
[[nodiscard]] bool wait_for_counter_checked(Counter* counter, std::int64_t target = 0);

/// Non-blocking poll; returns true iff the counter has already reached `target`. Callable from any thread.
[[nodiscard]] bool try_wait_for_counter(Counter* counter, std::int64_t target = 0) noexcept;

/// Park the current fiber until `counter` reaches `target` OR `deadline` passes; MUST be called from a fiber/job.
[[nodiscard]] bool wait_for_counter_until(Counter* counter, std::int64_t target,
                                          std::chrono::steady_clock::time_point deadline);

/// Block the CALLING (non-fiber) thread until `counter` reaches `target`.
bool wait_for_counter_blocking(Counter* counter, std::int64_t target = 0);

/// As wait_for_counter_blocking, but with a deadline; returns true if the target was reached, false on timeout.
[[nodiscard]] bool wait_for_counter_blocking_until(Counter* counter, std::int64_t target,
                                                   std::chrono::steady_clock::time_point deadline);

/// Run fn(i) for each i in [begin, end), split into chunks of `grain`, each a job, then join; MUST be called from a fiber/job.
template <class Index, class Fn>
void parallel_for(Index begin, Index end, Index grain, Fn&& fn) {
    if (begin >= end) return;
    JAF_ASSERT(grain > Index{0}, "jaf::parallel_for grain must be positive");

    Counter c;
    std::vector<Task> tasks;
    for (Index chunk_begin = begin; chunk_begin < end; chunk_begin += grain) {
        const Index chunk_end = (end - chunk_begin > grain) ? (chunk_begin + grain) : end;
        tasks.emplace_back([chunk_begin, chunk_end, &fn] {
            for (Index i = chunk_begin; i < chunk_end; ++i) fn(i);
        });
    }
    run(tasks, &c);
    wait_for_counter(&c, 0);
}

/// Auto-grain overload; targets ~kParallelForChunksPerWorker chunks per worker.
template <class Index, class Fn>
void parallel_for(Index begin, Index end, Fn&& fn) {
    if (begin >= end) return;
    const Index    total   = end - begin;
    const unsigned workers = detail::ActiveWorkerCount();
    const Index    chunks  = static_cast<Index>((workers ? workers : 1u) *
                                                config::kParallelForChunksPerWorker);
    Index grain = (total + chunks - Index{1}) / chunks;
    if (grain < Index{1}) grain = Index{1};
    parallel_for(begin, end, grain, std::forward<Fn>(fn));
}

} // namespace jaf
