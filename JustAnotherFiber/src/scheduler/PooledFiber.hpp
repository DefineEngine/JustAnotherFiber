#pragma once

// scheduler/PooledFiber.hpp -- a recyclable stackful fiber owned by the scheduler.

#include <jaf/Config.hpp>
#include <jaf/Scheduler.hpp>   // StackClass
#include <jaf/Task.hpp>

#include "context/FContext.hpp"
#include "scheduler/StackAlloc.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace jaf {

class Counter;

namespace detail {

class Worker;

/// @brief The lifecycle state of a pooled fiber.
enum class FiberState : std::uint8_t {
    Free,     ///< In the pool free-list, no assigned job.
    Running,  ///< Actively executing on a worker thread.
    Parked,   ///< Suspended on a counter's wait-list.
};

/// @brief A recyclable stackful fiber execution context.
struct alignas(config::kCacheLineSize) PooledFiber {
    fcontext_t   m_ctx        = nullptr; ///< Context to resume into this fiber.
    fcontext_t   m_worker_ctx = nullptr; ///< Worker dispatch context to switch back to.
    void*        m_stack      = nullptr; ///< Stack allocation base address.
    std::size_t  m_stack_size = 0;       ///< Stack allocation size.
    StackClass   m_class      = StackClass::Small; ///< Fiber size class.
    FiberState   m_state      = FiberState::Free;  ///< Current fiber lifecycle state.
    Priority     m_priority   = Priority::Normal;  ///< Priority of the running job (kept across parks).

    Task         m_job;                       ///< The current job to execute.
    Counter*     m_owning_counter = nullptr;  ///< Counter to decrement upon job completion.
    Counter*     m_wait_counter   = nullptr;  ///< Counter this fiber is suspended on.
    Waiter       m_waiter;                    ///< This fiber's wait-list node (m_waiter.fiber == this).
    bool         m_job_failed     = false;    ///< The current job threw (poison its counter).
    int          m_locks_held     = 0;        ///< jaf::Mutex locks held (debug migration check).
    Worker*      m_home           = nullptr;  ///< Pinned home worker; woken fiber returns here.
    std::atomic<bool>* m_cancel   = nullptr;  ///< Cancellation flag of the current job (borrowed).
    const char*        m_name     = nullptr;  ///< Profiler zone name of the current job (borrowed).

    // The "link into a wait-list" step, run on the worker stack after this fiber
    // switches off its own stack. Returns true if it parked the fiber, false if the
    // fiber must be rescheduled immediately (it became ready / acquired).
    bool (*m_park_link)(PooledFiber*, void*) noexcept = nullptr;
    void* m_park_ctx = nullptr;

    PooledFiber* m_next = nullptr;            ///< Intrusive link pointer for free/ready/wait lists.

    PooledFiber() noexcept { m_waiter.fiber = this; }
    ~PooledFiber() { FreeStack(m_stack, m_stack_size); }

    PooledFiber(const PooledFiber&)            = delete;
    PooledFiber& operator=(const PooledFiber&) = delete;
    PooledFiber(PooledFiber&&)                 = delete;
    PooledFiber& operator=(PooledFiber&&)      = delete;

    /// @brief The entry trampoline function that executes jobs in a loop.
    static void Trampoline(transfer_t t) noexcept;
};

} // namespace detail
} // namespace jaf
