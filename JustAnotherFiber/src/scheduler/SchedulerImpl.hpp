#pragma once

// scheduler/SchedulerImpl.hpp -- the guts behind jaf::Scheduler (pimpl).

#include "scheduler/FiberPool.hpp"
#include "scheduler/IoService.hpp"
#include "scheduler/Worker.hpp"

#include <jaf/Scheduler.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <vector>

namespace jaf::detail {

/// @brief A fiber parked with a deadline, threaded onto the scheduler's timer list.
struct TimedWaiter {
    Waiter                                w;                 ///< Counter wait-list node (w.wake = TimedWake).
    PooledFiber*                          fiber = nullptr;   ///< The parked fiber to reschedule.
    Counter*                              counter = nullptr; ///< Counter it waits on (for CounterTryUnlinkWaiter).
    std::chrono::steady_clock::time_point deadline{};        ///< Fire time.
    bool                                  timed_out = false; ///< Set by the wake winner; true iff the sweep won.
    bool                                  linked = false;    ///< In the timer list (guarded by timer_mutex).
    TimedWaiter*                          prev = nullptr;    ///< Timer list links (guarded by timer_mutex).
    TimedWaiter*                          next = nullptr;
};

/// @brief Internal scheduler implementation details.
struct SchedulerImpl {
    explicit SchedulerImpl(const SchedulerOptions& opts);
    ~SchedulerImpl();

    FiberPool                            pool;      ///< Pool of reusable fibers.
    std::vector<std::unique_ptr<Worker>> workers;   ///< List of spawned worker threads.
    std::unique_ptr<IoService>           io;        ///< Async file I/O (created in Start()).

    // One named worker per ThreadTag. Driven by pump(tag) on the owning engine
    // thread; NOT in `workers` (their pinned jobs are never stolen).
    std::array<std::unique_ptr<Worker>, static_cast<std::size_t>(ThreadTag::Count)> named_workers;

    std::atomic<std::int64_t> inflight{0};          ///< Number of in-flight jobs in the graph.
    std::atomic<std::int64_t> pending{0};           ///< Total runnable tasks (jobs + ready fibers).

    std::atomic<bool> draining{false};              ///< Flag indicating the scheduler is shutting down.
    std::atomic<unsigned> next_worker{0};           ///< Worker index for round-robin scheduling.
    std::atomic<unsigned> blocked_workers{0};       ///< Workers stalled on pool exhaustion (not idle).
    std::atomic<unsigned> active_loops{0};          ///< Dispatch loops currently running (bg + caller).
    std::atomic<unsigned>     idle_workers{0};      ///< Workers currently in WorkerIdleWait.
    std::atomic<std::int64_t> io_inflight{0};       ///< Outstanding async reads (excluded from deadlock check).
    unsigned bg_count = 0;                          ///< Background worker count (round-robin target set).
    bool caller_is_worker = true;                   ///< Caller of run() pumps instead of sleeping.
    bool pin_threads = false;                       ///< Pin workers to specific cores.
    bool enable_watchdog = false;                   ///< Opt-in deadlock watchdog.
    std::chrono::milliseconds pool_exhaustion_timeout{config::kPoolExhaustionTimeout}; ///< Exhaustion window.

    std::mutex              sleep_mutex;            ///< Mutex guarding worker parking condition variable.
    std::condition_variable work_cv;                ///< Condition variable where idle workers park.

    std::mutex              done_mutex;             ///< Mutex guarding main thread completion condition variable.
    std::condition_variable done_cv;                ///< Condition variable signaling task graph drainage.

    std::mutex         exc_mutex;                   ///< Mutex guarding exception capture.
    std::exception_ptr first_exception;             ///< The first captured exception escaping a job.

    // Timed-wait machinery. `timer_head` is a doubly-linked list of parked fibers
    // with deadlines; workers sweep it in their idle step. `timed_waiters` lets a
    // worker skip the sweep (and the mutex) entirely when none are armed.
    std::mutex            timer_mutex;              ///< Guards the timer list.
    TimedWaiter*          timer_head = nullptr;     ///< Head of the deadline list.
    std::atomic<unsigned> timed_waiters{0};         ///< Count of armed timed waits.

    /// @brief Spawn worker threads and optionally pin them.
    void Start();

    /// @brief Submit a single job to the queues.
    void Submit(JobItem&& item, Worker* from);

    /// @brief Place a woken fiber onto a worker's ready list.
    void ScheduleReady(PooledFiber* f) noexcept;

    /// @brief Decrement in-flight count and notify if the graph is drained.
    void OnJobComplete() noexcept;

    /// @brief Record an escaping exception from a job.
    void CaptureException(std::exception_ptr e) noexcept;

    /// @brief Notify one worker thread.
    void NotifyOne() noexcept { work_cv.notify_one(); }

    /// @brief Wake all worker threads.
    void WakeAllWorkers() noexcept { work_cv.notify_all(); }

    /// @brief Link a timed waiter into the deadline list.
    void RegisterTimer(TimedWaiter* t) noexcept;

    /// @brief Unlink a timed waiter from the deadline list (idempotent).
    void RemoveTimer(TimedWaiter* t) noexcept;

    /// @brief Wake any timed waiters whose deadline has passed and drop entries
    ///        already claimed by a completer. Called from the idle step.
    void SweepTimers() noexcept;

    /// @brief Park an idle worker thread waiting for new work.
    void WorkerIdleWait() noexcept;

    /// @brief Park a worker thread waiting for a free fiber.
    void WorkerBlockedWait() noexcept;

    /// @brief Number of background workers -- the round-robin target set for
    ///        Submit / ScheduleReady.
    unsigned WorkerCount() const noexcept { return bg_count; }

    /// @brief Total workers eligible as steal victims (background + caller).
    unsigned StealCount() const noexcept { return static_cast<unsigned>(workers.size()); }

    /// @brief The caller worker (index bg_count), or nullptr if disabled.
    Worker* CallerWorker() const noexcept {
        return caller_is_worker ? workers[bg_count].get() : nullptr;
    }

    /// @brief The named worker for a tag.
    Worker* NamedWorker(ThreadTag t) const noexcept {
        return named_workers[static_cast<std::size_t>(t)].get();
    }

    /// @brief True if the system looks stuck -- work outstanding, nothing runnable,
    ///        no async I/O pending, and every background worker idle.
    [[nodiscard]] bool LooksDeadlocked() const noexcept {
        return inflight.load(std::memory_order_acquire) > 0 &&
               pending.load(std::memory_order_acquire) == 0 &&
               io_inflight.load(std::memory_order_acquire) == 0 &&
               idle_workers.load(std::memory_order_acquire) >= bg_count;
    }

    /// @brief Print a rich stuck-state diagnostic to stderr and abort.
    [[noreturn]] void DumpStuckState(const char* reason) noexcept;
};

} // namespace jaf::detail
