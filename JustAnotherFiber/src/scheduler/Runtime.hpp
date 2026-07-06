#pragma once

// scheduler/Runtime.hpp -- cross-TU runtime accessors: the thread-locals, the
// active-scheduler pointer, and helpers the counter/trampoline code needs.

#include <exception>

namespace jaf::detail {

struct SchedulerImpl;
struct PooledFiber;
class Worker;

// Single active scheduler (set in Scheduler ctor, cleared in dtor).
void           SetActiveScheduler(SchedulerImpl* s) noexcept;
SchedulerImpl* ActiveScheduler() noexcept;

// Per-thread state.
void    SetCurrentWorker(Worker* w) noexcept;
Worker* CurrentWorker() noexcept;
void         SetCurrentFiber(PooledFiber* f) noexcept;
PooledFiber* CurrentFiber() noexcept;

// Push a woken/ready fiber onto a worker ready-queue (current worker if on one,
// else round-robin). Implemented in Scheduler.cpp.
void ScheduleReady(PooledFiber* f) noexcept;

// Park the current fiber. `link` runs on the worker stack AFTER the fiber is off
// its own stack; it links the fiber into some wait-list and returns true, or
// returns false if the fiber became ready and must be rescheduled at once.
using ParkLinkFn = bool (*)(PooledFiber*, void*) noexcept;
void ParkCurrentFiber(ParkLinkFn link, void* ctx) noexcept;

// Record the first exception escaping a job (first-wins). Implemented in
// Scheduler.cpp; surfaced from Scheduler::run.
void CaptureCurrentException(std::exception_ptr e) noexcept;

} // namespace jaf::detail
