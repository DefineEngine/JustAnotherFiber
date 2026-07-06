#pragma once

// jaf/Counter.hpp -- an atomic count-down dependency counter for fiber synchronization.

#include <jaf/Config.hpp>

#include <atomic>
#include <cstdint>
#include <thread>

namespace jaf {

class Counter;

namespace detail {

struct PooledFiber;

/// A lightweight spinlock guarding short critical sections.
class SpinLock {
public:
    /// Acquire the spinlock, block-spinning if it is currently held.
    void Lock() noexcept {
        unsigned spins = 0;
        while (m_flag.test_and_set(std::memory_order_acquire)) {
            do {
                if (++spins < config::kSpinBeforeYield) {
                    JAF_CPU_PAUSE();
                } else {
                    std::this_thread::yield();
                    spins = 0;
                }
            } while (m_flag.test(std::memory_order_relaxed));
        }
    }
    /// Release the spinlock.
    void Unlock() noexcept { m_flag.clear(std::memory_order_release); }

private:
    std::atomic_flag m_flag = ATOMIC_FLAG_INIT;
};

/// RAII guard to lock and unlock a SpinLock.
struct SpinGuard {
    explicit SpinGuard(SpinLock& l) noexcept : m_lock(l) { m_lock.Lock(); }
    ~SpinGuard() { m_lock.Unlock(); }
    SpinGuard(const SpinGuard&)            = delete;
    SpinGuard& operator=(const SpinGuard&) = delete;
    SpinLock& m_lock;
};

/// A node in a Counter's intrusive wait-list.
struct Waiter {
    Waiter*      next     = nullptr;   ///< Intrusive wait-list link (guarded by Counter::m_lock).
    std::int64_t target   = 0;         ///< Value the counter must reach to wake this waiter.
    PooledFiber* fiber    = nullptr;   ///< Fiber waiter: ScheduleReady on wake (fast path).
    void       (*wake)(void*) noexcept = nullptr;  ///< Non-fiber waiter: invoked on wake.
    void*        wake_ctx = nullptr;   ///< Context passed to `wake`.
};

void CounterAdd(Counter& c, std::int64_t n) noexcept;
void CounterCompleteN(Counter& c, std::int64_t n) noexcept;
void CounterCompleteOne(Counter& c) noexcept;
bool CounterAddWaiter(Counter& c, Waiter* w) noexcept;
bool CounterTryUnlinkWaiter(Counter& c, Waiter* w) noexcept;
bool CounterTrySatisfied(Counter& c, std::int64_t target) noexcept;
void CounterReset(Counter& c) noexcept;
void CounterPoison(Counter& c) noexcept;

} // namespace detail

/// An atomic dependency counter used for countdown synchronization between fibers.
class Counter {
public:
    /// Construct a counter with an initial value.
    explicit Counter(std::int64_t initial = 0) noexcept : m_value(initial) {}
    ~Counter();

    // Counters are pinned to memory; copying or moving is disabled.
    Counter(const Counter&)            = delete;
    Counter& operator=(const Counter&) = delete;
    Counter(Counter&&)                 = delete;
    Counter& operator=(Counter&&)      = delete;

    /// Get the current value of the counter.
    [[nodiscard]] std::int64_t value() const noexcept {
        return m_value.load(std::memory_order_acquire);
    }

    /// True if any job that decremented this counter threw.
    [[nodiscard]] bool poisoned() const noexcept {
        return m_poisoned.load(std::memory_order_acquire);
    }

private:
    friend void detail::CounterAdd(Counter&, std::int64_t) noexcept;
    friend void detail::CounterCompleteN(Counter&, std::int64_t) noexcept;
    friend void detail::CounterCompleteOne(Counter&) noexcept;
    friend bool detail::CounterAddWaiter(Counter&, detail::Waiter*) noexcept;
    friend bool detail::CounterTryUnlinkWaiter(Counter&, detail::Waiter*) noexcept;
    friend bool detail::CounterTrySatisfied(Counter&, std::int64_t) noexcept;
    friend void detail::CounterReset(Counter&) noexcept;
    friend void detail::CounterPoison(Counter&) noexcept;

    std::atomic<std::int64_t> m_value;
    detail::SpinLock          m_lock;          ///< Lock guarding the wait-list access.
    detail::Waiter*           m_waiters = nullptr; ///< Head of the wait-list (fiber/blocking/coroutine).
    std::atomic<bool>         m_poisoned{false}; ///< A job on this counter threw.
};

/// Decrement `c` by `n` and wake any waiters it now satisfies. Safe to call from any thread.
void signal(Counter& c, std::int64_t n = 1) noexcept;

} // namespace jaf
