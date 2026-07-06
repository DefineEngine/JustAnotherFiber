#pragma once

// scheduler/Worker.hpp -- one OS thread running a fiber-dispatch loop.

#include <jaf/Config.hpp>      // config::kPriorityLevels
#include <jaf/Scheduler.hpp>   // StackClass, Priority
#include <jaf/Task.hpp>

#include "context/FContext.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

namespace jaf {
class Counter;
namespace detail {

struct SchedulerImpl;
struct PooledFiber;

/// @brief Represents a task entry in the worker's execution queue.
struct JobItem {
    Task               job;                     ///< The type-erased callable task.
    Counter*           counter = nullptr;       ///< Optional counter to decrement on completion.
    StackClass         cls     = StackClass::Small; ///< Stack size class required for the task.
    Priority           prio    = Priority::Normal;  ///< Which priority queue this job waits in.
    std::atomic<bool>* cancel  = nullptr;       ///< Cancellation flag (borrowed from a token).
    const char*        name    = nullptr;       ///< Optional profiler zone name (borrowed).
};

/// @brief Manages an OS thread running the fiber execution and task dispatch loop.
class Worker {
public:
    /// @brief Construct a worker instance associated with a scheduler.
    Worker(SchedulerImpl* sched, unsigned index) noexcept;

    /// @brief Spawn the underlying OS thread and begin execution.
    void Start();

    /// @brief Block until the worker thread exits.
    void Join();

    /// @brief Bind the worker thread to a specific logical processor.
    void Pin(unsigned core) noexcept;

    /// @brief Get the unique worker thread index.
    unsigned Index() const noexcept { return m_index; }

    /// @brief Jobs completed on this worker (for load-balance profiling).
    [[nodiscard]] std::uint64_t JobsRun() const noexcept {
        return m_jobs_run.load(std::memory_order_relaxed);
    }
    /// @brief Successful steals by this worker.
    [[nodiscard]] std::uint64_t Steals() const noexcept {
        return m_steals.load(std::memory_order_relaxed);
    }

    /// @brief Push a ready fiber onto the worker's execution queue.
    void PushFiber(PooledFiber* f);

    /// @brief Push a new job onto the worker's queue.
    void PushJob(JobItem&& item);

    /// @brief Steal a ready fiber from this worker's queue.
    PooledFiber* StealFiber() noexcept;

    /// @brief Steal a job from this worker's queue.
    bool         StealJob(JobItem& out) noexcept;

    /// @brief Drive the dispatch loop on the CALLING thread until the graph
    ///        drains (inflight == 0). Used for the caller worker in Scheduler::run.
    void PumpUntilDrained();

    /// @brief Run this (named) worker's currently-ready jobs/fibers ONCE on the
    ///        calling thread, then return. No stealing, no idle-wait.
    void DrainOnce();

private:
    /// @brief Background-thread dispatch loop entry (runs until draining & quiescent).
    void Run();

    /// @brief Shared dispatch loop body. `caller` selects the exit condition:
    ///        inflight==0 (caller) vs draining && pending==0 (background).
    void RunLoop(bool caller);

    /// @brief Resume execution of a fiber on this worker's context.
    void ResumeFiber(PooledFiber* f) noexcept;

    /// @brief Attempt to steal work from other sibling workers.
    bool TrySteal() noexcept;

    /// @brief Pop a ready fiber from our queue (LIFO for cache locality).
    PooledFiber* PopFiber() noexcept;

    /// @brief Pop a job from our queue (FIFO for fairness).
    bool         PopJob(JobItem& out) noexcept;

    SchedulerImpl* m_sched;
    unsigned       m_index;
    std::thread    m_thread;

    std::atomic<std::uint64_t> m_jobs_run{0};   ///< Jobs completed here.
    std::atomic<std::uint64_t> m_steals{0};     ///< Successful steals.

    // One queue per priority level, scanned High(0) -> Normal(1) -> Low(2).
    std::mutex m_mutex;   // guards every queue below
    std::array<std::deque<PooledFiber*>, config::kPriorityLevels> m_ready_fibers;
    std::array<std::deque<JobItem>,      config::kPriorityLevels> m_ready_jobs;
};

} // namespace detail
} // namespace jaf
