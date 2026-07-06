#pragma once

// jaf/Coro.hpp -- C++20 coroutine layer over the scheduler. A suspended
// coroutine holds no fiber-pool slot, unlike a parked fiber.

#include <jaf/Config.hpp>
#include <jaf/Counter.hpp>
#include <jaf/Scheduler.hpp>
#include <jaf/Graph.hpp>
#include <jaf/Io.hpp>

#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <utility>

namespace jaf {

namespace detail {

void CoroInflightBegin() noexcept;
void CoroInflightEnd() noexcept;
void CaptureCurrentException(std::exception_ptr) noexcept;

inline void ScheduleResume(std::coroutine_handle<> h) {
    Task t{[h]() { std::coroutine_handle<>(h).resume(); }};
    jaf::run(std::span<Task>{&t, 1}, /*counter*/ nullptr);
}

// The coroutine frame may migrate threads, so this node is heap-owned rather
// than living on a stack; freed by Wake (or the awaiter, if never armed).
struct CoroWaitNode {
    Waiter                  w;
    std::coroutine_handle<> handle;

    static void Wake(void* ctx) noexcept {
        auto* n = static_cast<CoroWaitNode*>(ctx);
        auto  h = n->handle;   // copy before freeing the node
        delete n;
        ScheduleResume(h);
    }
};

// Returns true if it suspended; false if `c` was already satisfied.
inline bool ArmCoroWait(Counter& c, std::int64_t target, std::coroutine_handle<> h) {
    auto* n     = new CoroWaitNode{};
    n->handle   = h;
    n->w.target = target;
    n->w.fiber  = nullptr;
    n->w.wake   = &CoroWaitNode::Wake;
    n->w.wake_ctx = n;
    if (!CounterAddWaiter(c, &n->w)) {   // already satisfied between poll and now
        delete n;
        return false;
    }
    return true;
}

struct CoroPromiseBase {
    std::coroutine_handle<> continuation{};
    std::exception_ptr      exception{};

    std::suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
        [[nodiscard]] bool await_ready() const noexcept { return false; }
        template <class P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
            std::coroutine_handle<> cont = h.promise().continuation;
            return cont ? cont : std::noop_coroutine();
        }
        void await_resume() const noexcept {}
    };
    FinalAwaiter final_suspend() noexcept { return {}; }

    void unhandled_exception() noexcept { exception = std::current_exception(); }
};

} // namespace detail

// ---- Coro<T> ----------------------------------------------------------------

/// A lazy coroutine returning T. Move-only; owns its coroutine frame.
template <class T>
class Coro {
public:
    struct promise_type : detail::CoroPromiseBase {
        T value{};
        Coro get_return_object() noexcept {
            return Coro{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        template <class U = T>
        void return_value(U&& v) { value = std::forward<U>(v); }
    };
    using handle_t = std::coroutine_handle<promise_type>;

    Coro() noexcept = default;
    explicit Coro(handle_t h) noexcept : m_h(h) {}
    Coro(Coro&& o) noexcept : m_h(o.m_h) { o.m_h = {}; }
    Coro& operator=(Coro&& o) noexcept {
        if (this != &o) { if (m_h) m_h.destroy(); m_h = o.m_h; o.m_h = {}; }
        return *this;
    }
    Coro(const Coro&)            = delete;
    Coro& operator=(const Coro&) = delete;
    ~Coro() { if (m_h) m_h.destroy(); }

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(m_h); }
    [[nodiscard]] handle_t handle() const noexcept { return m_h; }

    /// Await this coroutine from inside another coroutine; yields its value.
    auto operator co_await() const noexcept {
        struct Awaiter {
            handle_t child;
            [[nodiscard]] bool await_ready() const noexcept { return !child || child.done(); }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) const noexcept {
                child.promise().continuation = parent;
                return child;   // symmetric transfer: start/resume the child now
            }
            T await_resume() const {
                if (child.promise().exception) std::rethrow_exception(child.promise().exception);
                return std::move(child.promise().value);
            }
        };
        return Awaiter{m_h};
    }

private:
    handle_t m_h{};
};

/// Coro specialization for coroutines returning void.
template <>
class Coro<void> {
public:
    struct promise_type : detail::CoroPromiseBase {
        Coro get_return_object() noexcept {
            return Coro{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        void return_void() noexcept {}
    };
    using handle_t = std::coroutine_handle<promise_type>;

    Coro() noexcept = default;
    explicit Coro(handle_t h) noexcept : m_h(h) {}
    Coro(Coro&& o) noexcept : m_h(o.m_h) { o.m_h = {}; }
    Coro& operator=(Coro&& o) noexcept {
        if (this != &o) { if (m_h) m_h.destroy(); m_h = o.m_h; o.m_h = {}; }
        return *this;
    }
    Coro(const Coro&)            = delete;
    Coro& operator=(const Coro&) = delete;
    ~Coro() { if (m_h) m_h.destroy(); }

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(m_h); }
    [[nodiscard]] handle_t handle() const noexcept { return m_h; }

    auto operator co_await() const noexcept {
        struct Awaiter {
            handle_t child;
            [[nodiscard]] bool await_ready() const noexcept { return !child || child.done(); }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) const noexcept {
                child.promise().continuation = parent;
                return child;
            }
            void await_resume() const {
                if (child.promise().exception) std::rethrow_exception(child.promise().exception);
            }
        };
        return Awaiter{m_h};
    }

private:
    handle_t m_h{};
};

// ---- co_spawn ---------------------------------------------------------------

namespace detail {

struct DetachedTask {
    struct promise_type {
        DetachedTask get_return_object() noexcept {
            return DetachedTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_never  final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { CaptureCurrentException(std::current_exception()); }
    };
    std::coroutine_handle<promise_type> h;
};

template <class T>
DetachedTask CoSpawnDriver(Coro<T> coro, Counter* done) {
    // Runs on the frame's unwind path too, so `done` is signalled and the
    // inflight hold released even if the coroutine throws.
    struct End {
        Counter* d;
        ~End() { if (d) jaf::signal(*d); CoroInflightEnd(); }
    } end{done};
    co_await std::move(coro);
}

} // namespace detail

/// Launch `coro` as a detached root coroutine on the scheduler; signals `done`
/// (if non-null) on completion. A coroutine that throws surfaces from Scheduler::run.
template <class T>
void co_spawn(Coro<T> coro, Counter* done = nullptr) {
    if (!coro) { if (done) jaf::signal(*done); return; }
    detail::CoroInflightBegin();                                   // hold inflight until completion
    detail::DetachedTask d = detail::CoSpawnDriver(std::move(coro), done);
    detail::ScheduleResume(d.h);                                   // first resume on a worker
}

// ---- co_await a Counter -----------------------------------------------------

/// Awaiter that suspends the current coroutine until a counter is reached.
struct CounterAwaiter {
    Counter*     counter;
    std::int64_t target;

    [[nodiscard]] bool await_ready() const noexcept {
        return jaf::try_wait_for_counter(counter, target);
    }
    bool await_suspend(std::coroutine_handle<> h) const {
        return detail::ArmCoroWait(*counter, target, h);   // false -> satisfied, don't suspend
    }
    void await_resume() const noexcept {}
};

/// `co_await counter;` waits until the counter reaches 0.
inline CounterAwaiter operator co_await(Counter& c) noexcept { return {&c, 0}; }

/// `co_await jaf::until(counter, target);` waits until it reaches `target`.
[[nodiscard]] inline CounterAwaiter until(Counter& c, std::int64_t target) noexcept {
    return {&c, target};
}

// ---- co_await a GraphRun ----------------------------------------------------

/// `co_await schedule_async(g);` awaits a whole DAG from a coroutine.
inline auto operator co_await(GraphRun& run) {
    struct Awaiter {
        GraphRun* run;
        [[nodiscard]] bool await_ready() const noexcept { return run->done(); }
        bool await_suspend(std::coroutine_handle<> h) const {
            return detail::ArmCoroWait(run->await_counter(), 0, h);
        }
        void await_resume() const { run->finalize_completed(); }   // rethrow / completeness / consume
    };
    return Awaiter{&run};
}
inline auto operator co_await(GraphRun&& run) { return operator co_await(run); }

} // namespace jaf

// ---- co_await jaf::io::co_read ---------------------------------------------

namespace jaf::io {

/// Awaitable async read: `auto r = co_await jaf::io::co_read({path, buf});`
/// posts the read, suspends the coroutine, and returns the IoResult.
class ReadAwaitable {
public:
    explicit ReadAwaitable(const ReadRequest& req)
        : m_req(req), m_counter(std::make_unique<Counter>()) {}

    [[nodiscard]] bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
        jaf::io::read_file(m_req, m_counter.get(), &m_result);
        return jaf::detail::ArmCoroWait(*m_counter, 0, h);
    }
    IoResult await_resume() const noexcept { return m_result; }

private:
    ReadRequest              m_req;
    std::unique_ptr<Counter> m_counter;   // unique_ptr: Counter is immovable, awaitable must move
    IoResult                 m_result{};
};

/// Build an awaitable read (see ReadAwaitable).
[[nodiscard]] inline ReadAwaitable co_read(const ReadRequest& req) { return ReadAwaitable{req}; }

} // namespace jaf::io
