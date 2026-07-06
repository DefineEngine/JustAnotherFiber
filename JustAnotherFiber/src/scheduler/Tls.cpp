#include "scheduler/Runtime.hpp"

#include <atomic>

namespace jaf::detail {

namespace {
// Read from arbitrary threads, so atomic rather than a plain pointer: release on
// publish / acquire on read.
std::atomic<SchedulerImpl*> g_active_scheduler{nullptr};

thread_local Worker*      tl_worker = nullptr;
thread_local PooledFiber* tl_fiber  = nullptr;
} // namespace

void SetActiveScheduler(SchedulerImpl* s) noexcept {
    g_active_scheduler.store(s, std::memory_order_release);
}
SchedulerImpl* ActiveScheduler() noexcept {
    return g_active_scheduler.load(std::memory_order_acquire);
}

void    SetCurrentWorker(Worker* w) noexcept { tl_worker = w; }
Worker* CurrentWorker() noexcept { return tl_worker; }

void         SetCurrentFiber(PooledFiber* f) noexcept { tl_fiber = f; }
PooledFiber* CurrentFiber() noexcept { return tl_fiber; }

} // namespace jaf::detail
