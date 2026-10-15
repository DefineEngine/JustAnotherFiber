#pragma once

// jaf/Sync.hpp -- fiber-aware synchronization primitives whose waits park the
// fiber, leaving the worker free to run other jobs.

#include <jaf/Config.hpp>
#include <jaf/Counter.hpp>

#include <cstdint>

namespace jaf {

/// A fiber-owned mutual exclusion lock; lock() parks the fiber if held, unlock() is callable from any thread.
class Mutex {
public:
    Mutex() noexcept = default;
    ~Mutex() = default;
    Mutex(const Mutex&)            = delete;
    Mutex& operator=(const Mutex&) = delete;

    JAF_API void lock();          ///< Fiber-only; parks if the lock is held.
    JAF_API bool try_lock();      ///< Never parks; true if acquired.
    JAF_API void unlock();        ///< Release; wakes one waiter. Legal from any thread.

private:
    static bool LinkLock(detail::PooledFiber* self, void* ctx) noexcept;

    detail::SpinLock     m_lock;                ///< Guards the fields below.
    detail::PooledFiber* m_owner   = nullptr;   ///< Owning fiber (not thread).
    detail::PooledFiber* m_waiters = nullptr;   ///< Parked contenders (intrusive m_next).
    bool                 m_held    = false;
};

/// A manual-reset event; signal() wakes ALL current and future waiters until reset().
class ManualResetEvent {
public:
    ManualResetEvent() noexcept = default;
    ManualResetEvent(const ManualResetEvent&)            = delete;
    ManualResetEvent& operator=(const ManualResetEvent&) = delete;

    JAF_API void wait();    ///< Fiber-only; parks until set.
    JAF_API void signal();  ///< Set and wake all waiters. Legal from any thread.
    JAF_API void reset();   ///< Clear the set state.

private:
    static bool LinkWait(detail::PooledFiber* self, void* ctx) noexcept;

    detail::SpinLock     m_lock;
    detail::PooledFiber* m_waiters = nullptr;
    bool                 m_set     = false;
};

/// An auto-reset event; signal() wakes exactly ONE waiter, else leaves a pending signal for the next wait().
class Event {
public:
    Event() noexcept = default;
    Event(const Event&)            = delete;
    Event& operator=(const Event&) = delete;

    JAF_API void wait();    ///< Fiber-only; parks until signalled, consuming the signal.
    JAF_API void signal();  ///< Wake one waiter, else leave a pending signal. Any thread.

private:
    static bool LinkWait(detail::PooledFiber* self, void* ctx) noexcept;

    detail::SpinLock     m_lock;
    detail::PooledFiber* m_waiters = nullptr;
    bool                 m_set     = false;
};

/// A counting semaphore; acquire() parks if no permit is available, release(n) returns n permits.
class Semaphore {
public:
    explicit Semaphore(std::int64_t initial) noexcept : m_count(initial) {}
    Semaphore(const Semaphore&)            = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    JAF_API void acquire();                       ///< Fiber-only; parks if count == 0.
    JAF_API void release(std::int64_t n = 1);     ///< Return n permits. Legal from any thread.

private:
    static bool LinkAcquire(detail::PooledFiber* self, void* ctx) noexcept;

    detail::SpinLock     m_lock;
    std::int64_t         m_count   = 0;      ///< Available permits (guarded by m_lock).
    detail::PooledFiber* m_waiters = nullptr;
};

} // namespace jaf
