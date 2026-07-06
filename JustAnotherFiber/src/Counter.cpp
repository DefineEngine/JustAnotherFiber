// src/Counter.cpp -- jaf::Counter runtime operations.

#include <jaf/Counter.hpp>

#include "scheduler/PooledFiber.hpp"
#include "scheduler/Runtime.hpp"

#include <cstdint>

namespace jaf
{
    Counter::~Counter()
    {
        JAF_ASSERT(m_waiters == nullptr,
                   "Counter destroyed while fibers are still parked on it");
    }

    namespace detail
    {
        static bool Satisfied(std::int64_t value, std::int64_t target) noexcept
        {
            return value <= target;
        }

        void CounterAdd(Counter& c, std::int64_t n) noexcept
        {
            c.m_value.fetch_add(n, std::memory_order_relaxed);
        }

        void CounterPoison(Counter& c) noexcept
        {
            c.m_poisoned.store(true, std::memory_order_release);
        }

        bool CounterAddWaiter(Counter& c, Waiter* w) noexcept
        {
            SpinGuard g(c.m_lock);
            // Re-check under the lock: the counter may have hit the target between the
            // caller's fast-path read and now. If so, do NOT link -- the caller self-wakes.
            if (Satisfied(c.m_value.load(std::memory_order_acquire), w->target))
            {
                return false;
            }
            w->next = c.m_waiters;
            c.m_waiters = w;
            return true;
        }

        bool CounterTryUnlinkWaiter(Counter& c, Waiter* target) noexcept
        {
            SpinGuard g(c.m_lock);
            for (Waiter** pp = &c.m_waiters; *pp; pp = &(*pp)->next)
            {
                if (*pp == target)
                {
                    *pp = target->next;
                    target->next = nullptr;
                    return true;
                }
            }
            return false;
        }

        bool CounterTrySatisfied(Counter& c, std::int64_t target) noexcept
        {
            SpinGuard g(c.m_lock);
            return Satisfied(c.m_value.load(std::memory_order_acquire), target);
        }

        void CounterReset(Counter& c) noexcept
        {
            JAF_ASSERT(c.m_waiters == nullptr, "CounterReset with waiters still linked");
            c.m_value.store(0, std::memory_order_relaxed);
            c.m_poisoned.store(false, std::memory_order_relaxed);
        }

        void CounterCompleteN(Counter& c, std::int64_t n) noexcept
        {
            Waiter* woken = nullptr;
            {
                SpinGuard g(c.m_lock);
                // decrement under the lock so the completer that drives the value to
                // the target is the last to touch this Counter
                const std::int64_t v = c.m_value.fetch_sub(n, std::memory_order_relaxed) - n;
                Waiter** pp = &c.m_waiters;
                while (*pp)
                {
                    Waiter* nd = *pp;
                    if (Satisfied(v, nd->target))
                    {
                        *pp = nd->next; // unlink
                        nd->next = woken; // splice into local wake-list
                        woken = nd;
                    }
                    else
                    {
                        pp = &nd->next;
                    }
                }
            }

            for (Waiter* w = woken; w != nullptr;)
            {
                Waiter* next = w->next;
                w->next = nullptr;
                if (w->fiber != nullptr)
                {
                    w->fiber->m_wait_counter = nullptr;
                    ScheduleReady(w->fiber); // push onto a worker ready-queue (hot path)
                }
                else
                {
                    w->wake(w->wake_ctx);
                }
                w = next;
            }
        }

        void CounterCompleteOne(Counter& c) noexcept { CounterCompleteN(c, 1); }
    } // namespace detail

    void signal(Counter& c, std::int64_t n) noexcept
    {
        JAF_ASSERT(n > 0, "jaf::signal count must be positive");
        detail::CounterCompleteN(c, n);
    }
} // namespace jaf
