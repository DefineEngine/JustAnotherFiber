#include "scheduler/FiberPool.hpp"

#include "scheduler/Runtime.hpp"
#include "scheduler/Sentinels.hpp"
#include "scheduler/StackAlloc.hpp"
#include "context/FContext.hpp"

#include <cstddef>
#include <cstdio>
#include <exception>
#include <utility>

namespace jaf::detail {

FiberPool::FiberPool(std::size_t small_count, std::size_t large_count)
    : m_small_total(small_count),
      m_large_total(large_count),
      m_small_free_count(small_count),
      m_large_free_count(large_count) {
    m_storage.reserve(small_count + large_count);
    for (std::size_t i = 0; i < small_count; ++i) {
        PooledFiber* f = MakeFiber(StackClass::Small);
        f->m_next = m_small_free;
        m_small_free = f;
    }
    for (std::size_t i = 0; i < large_count; ++i) {
        PooledFiber* f = MakeFiber(StackClass::Large);
        f->m_next = m_large_free;
        m_large_free = f;
    }
}

PooledFiber* FiberPool::MakeFiber(StackClass cls) {
    auto owner = std::make_unique<PooledFiber>();
    PooledFiber* f = owner.get();
    m_storage.push_back(std::move(owner));

    const std::size_t size =
        (cls == StackClass::Large) ? config::kLargeStackSize : config::kDefaultStackSize;

    // base/total identify the block for ~PooledFiber; top/usable feed make_fcontext
    // (which wants the highest address -- the stack grows down).
    const StackAlloc sa = AllocStack(size);
    f->m_stack      = sa.base;
    f->m_stack_size = sa.total;
    f->m_class      = cls;
    f->m_ctx        = make_fcontext(sa.top, sa.usable, &PooledFiber::Trampoline);
    return f;
}

PooledFiber* FiberPool::Acquire(StackClass cls) noexcept {
    std::lock_guard<std::mutex> lk(m_mutex);
    PooledFiber* f = nullptr;
    if (cls == StackClass::Large) {
        if ((f = m_large_free)) { m_large_free = f->m_next; --m_large_free_count; }
    } else {
        if ((f = m_small_free)) {
            m_small_free = f->m_next;
            --m_small_free_count;
        } else if ((f = m_large_free)) {
            // A Small request may borrow a free Large fiber: a Large stack is a
            // strict superset, so this is always safe (never the reverse). Release
            // routes by f->m_class, so a borrowed Large fiber returns Large.
            m_large_free = f->m_next;
            --m_large_free_count;
        }
    }
    if (f) {
        f->m_next  = nullptr;
        f->m_state = FiberState::Running;
    }
    return f;
}

void FiberPool::Release(PooledFiber* f) noexcept {
    f->m_job.reset();
    f->m_owning_counter = nullptr;
    f->m_wait_counter   = nullptr;
    f->m_job_failed     = false;
    f->m_home           = nullptr;
    f->m_cancel         = nullptr;
    f->m_name           = nullptr;
    f->m_state          = FiberState::Free;

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (f->m_class == StackClass::Large) {
            f->m_next = m_large_free;
            m_large_free = f;
            ++m_large_free_count;
        } else {
            f->m_next = m_small_free;
            m_small_free = f;
            ++m_small_free_count;
        }
    }
    // Publish progress so the watchdog can distinguish a live pool from a livelock.
    m_release_gen.fetch_add(1, std::memory_order_relaxed);
}

FiberPool::ClassOccupancy FiberPool::Occupancy(StackClass cls) noexcept {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (cls == StackClass::Large) {
        return ClassOccupancy{m_large_total, m_large_total - m_large_free_count};
    }
    return ClassOccupancy{m_small_total, m_small_total - m_small_free_count};
}

void FiberPool::ReportExhaustion(StackClass cls) noexcept {
    const ClassOccupancy occ = Occupancy(cls);
    const char* const cls_name = (cls == StackClass::Large) ? "Large" : "Small";
    const char* const opt_name = (cls == StackClass::Large) ? "large" : "small";
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "fiber pool exhausted by parked fibers (class=%s, size=%zu, in_use=%zu) -- "
                  "raise SchedulerOptions::%s_fibers or reduce concurrently-parked nested waits",
                  cls_name, occ.total, occ.in_use, opt_name);
    JAF_ASSERT(false, buf);
}

// The looping trampoline. Entered once via make_fcontext; runs jobs forever,
// switching back to its worker (kFinished) between them. Never returns.
void PooledFiber::Trampoline(transfer_t t) noexcept {
    auto* self = static_cast<PooledFiber*>(t.data);
    self->m_worker_ctx = t.fctx;

    for (;;) {
        JAF_PROFILE_JOB_BEGIN(self->m_name);
        if (self->m_cancel == nullptr ||
            !self->m_cancel->load(std::memory_order_acquire)) {
            try {
                self->m_job();
            } catch (...) {
                // Exceptions must never unwind across an fcontext boundary.
                CaptureCurrentException(std::current_exception());
                self->m_job_failed = true;
            }
        }
        JAF_PROFILE_JOB_END();
        self->m_job.reset();

        transfer_t back = jump_fcontext(self->m_worker_ctx,
                                        reinterpret_cast<void*>(kFinished));
        self               = static_cast<PooledFiber*>(back.data);
        self->m_worker_ctx = back.fctx;
    }
}

} // namespace jaf::detail
