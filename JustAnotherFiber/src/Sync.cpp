// src/Sync.cpp -- fiber-aware Mutex / Event / ManualResetEvent / Semaphore.

#include <jaf/Sync.hpp>

#include "scheduler/PooledFiber.hpp"
#include "scheduler/Runtime.hpp"

namespace jaf {

namespace {
// Splice a whole wait-list into ready-queues (used by broadcast wakes).
void ScheduleList(detail::PooledFiber* head) noexcept {
    while (head != nullptr) {
        detail::PooledFiber* next = head->m_next;
        head->m_next = nullptr;
        detail::ScheduleReady(head);
        head = next;
    }
}
} // namespace

// ---- Mutex -----------------------------------------------------------------

bool Mutex::LinkLock(detail::PooledFiber* self, void* ctx) noexcept {
    Mutex* m = static_cast<Mutex*>(ctx);
    detail::SpinGuard g(m->m_lock);
    if (!m->m_held) {                      // became free between fast path and here
        m->m_held  = true;
        m->m_owner = self;
        return false;                      // acquired -> reschedule, do not park
    }
    self->m_next  = m->m_waiters;          // park on the wait-list
    m->m_waiters  = self;
    return true;
}

void Mutex::lock() {
    detail::PooledFiber* self = detail::CurrentFiber();
    JAF_ASSERT(self != nullptr, "jaf::Mutex::lock must be called from inside a fiber");

    bool acquired = false;
    {
        detail::SpinGuard g(m_lock);
        if (!m_held) { m_held = true; m_owner = self; acquired = true; }
    }
    if (!acquired) {
        detail::ParkCurrentFiber(&Mutex::LinkLock, this);  // resumes owning the lock
    }
#ifndef NDEBUG
    ++self->m_locks_held;
#endif
}

bool Mutex::try_lock() {
    detail::PooledFiber* self = detail::CurrentFiber();
    detail::SpinGuard g(m_lock);
    if (m_held) return false;
    m_held  = true;
    m_owner = self;
#ifndef NDEBUG
    if (self != nullptr) ++self->m_locks_held;
#endif
    return true;
}

void Mutex::unlock() {
    detail::PooledFiber* next = nullptr;
    {
        detail::SpinGuard g(m_lock);
        JAF_ASSERT(m_held, "jaf::Mutex::unlock of an unlocked mutex");
#ifndef NDEBUG
        if (detail::PooledFiber* cur = detail::CurrentFiber()) {
            JAF_ASSERT(m_owner == cur, "jaf::Mutex::unlock from a fiber that does not own it");
        }
#endif
        if (m_waiters != nullptr) {
            next         = m_waiters;
            m_waiters    = next->m_next;
            next->m_next = nullptr;
            m_owner      = next;      // hand ownership to the woken waiter (stays held)
        } else {
            m_held  = false;
            m_owner = nullptr;
        }
    }
#ifndef NDEBUG
    if (detail::PooledFiber* cur = detail::CurrentFiber()) --cur->m_locks_held;
#endif
    if (next != nullptr) detail::ScheduleReady(next);
}

// ---- ManualResetEvent ------------------------------------------------------

bool ManualResetEvent::LinkWait(detail::PooledFiber* self, void* ctx) noexcept {
    auto* e = static_cast<ManualResetEvent*>(ctx);
    detail::SpinGuard g(e->m_lock);
    if (e->m_set) return false;             // set meanwhile -> reschedule
    self->m_next = e->m_waiters;
    e->m_waiters = self;
    return true;
}

void ManualResetEvent::wait() {
    JAF_ASSERT(detail::CurrentFiber() != nullptr,
               "jaf::ManualResetEvent::wait must be called from inside a fiber");
    {
        detail::SpinGuard g(m_lock);
        if (m_set) return;
    }
    detail::ParkCurrentFiber(&ManualResetEvent::LinkWait, this);
}

void ManualResetEvent::signal() {
    detail::PooledFiber* woken = nullptr;
    {
        detail::SpinGuard g(m_lock);
        m_set     = true;
        woken     = m_waiters;
        m_waiters = nullptr;
    }
    ScheduleList(woken);   // wake ALL
}

void ManualResetEvent::reset() {
    detail::SpinGuard g(m_lock);
    m_set = false;
}

// ---- Event (auto-reset) ----------------------------------------------------

bool Event::LinkWait(detail::PooledFiber* self, void* ctx) noexcept {
    auto* e = static_cast<Event*>(ctx);
    detail::SpinGuard g(e->m_lock);
    if (e->m_set) { e->m_set = false; return false; }   // consume pending -> reschedule
    self->m_next = e->m_waiters;
    e->m_waiters = self;
    return true;
}

void Event::wait() {
    JAF_ASSERT(detail::CurrentFiber() != nullptr,
               "jaf::Event::wait must be called from inside a fiber");
    {
        detail::SpinGuard g(m_lock);
        if (m_set) { m_set = false; return; }   // consume a pending signal
    }
    detail::ParkCurrentFiber(&Event::LinkWait, this);
}

void Event::signal() {
    detail::PooledFiber* one = nullptr;
    {
        detail::SpinGuard g(m_lock);
        if (m_waiters != nullptr) {
            one         = m_waiters;
            m_waiters   = one->m_next;
            one->m_next = nullptr;
        } else {
            m_set = true;   // no waiter: leave a pending signal for the next wait()
        }
    }
    if (one != nullptr) detail::ScheduleReady(one);
}

// ---- Semaphore -------------------------------------------------------------

bool Semaphore::LinkAcquire(detail::PooledFiber* self, void* ctx) noexcept {
    auto* s = static_cast<Semaphore*>(ctx);
    detail::SpinGuard g(s->m_lock);
    if (s->m_count > 0) { --s->m_count; return false; }   // got a permit -> reschedule
    self->m_next = s->m_waiters;
    s->m_waiters = self;
    return true;
}

void Semaphore::acquire() {
    JAF_ASSERT(detail::CurrentFiber() != nullptr,
               "jaf::Semaphore::acquire must be called from inside a fiber");
    {
        detail::SpinGuard g(m_lock);
        if (m_count > 0) { --m_count; return; }
    }
    detail::ParkCurrentFiber(&Semaphore::LinkAcquire, this);
}

void Semaphore::release(std::int64_t n) {
    JAF_ASSERT(n > 0, "jaf::Semaphore::release count must be positive");
    detail::PooledFiber* woken = nullptr;
    {
        detail::SpinGuard g(m_lock);
        for (std::int64_t i = 0; i < n; ++i) {
            if (m_waiters != nullptr) {
                // Hand the permit directly to a waiter (it parked without taking a
                // permit); do NOT bump m_count for it.
                detail::PooledFiber* w = m_waiters;
                m_waiters    = w->m_next;
                w->m_next    = woken;
                woken        = w;
            } else {
                ++m_count;
            }
        }
    }
    ScheduleList(woken);
}

} // namespace jaf
