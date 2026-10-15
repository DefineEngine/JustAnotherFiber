#pragma once

// jaf/Fiber.hpp -- a cooperatively scheduled execution context with its own stack.

#include <jaf/Config.hpp>
#include <jaf/Task.hpp>

#include <cstddef>
#include <exception>
#include <utility>

namespace jaf {

// Forward declarations so the public header need not expose the private
// FContext.hpp. `transfer_t` is only named (by value) in a declaration here,
// which permits it to be incomplete; it is completed in the .cpp.
namespace detail { struct transfer_t; }

namespace this_fiber {
/// Suspend the currently running fiber, returning control to whoever resumed it.
JAF_API void yield() noexcept;
} // namespace this_fiber

/// A cooperatively scheduled execution context with its own stack.
class Fiber {
public:
    /// The execution state of a fiber.
    enum class State {
        Ready,      ///< Constructed, never entered.
        Running,    ///< Currently executing.
        Suspended,  ///< Yielded, waiting to be resumed.
        Done,       ///< Entry function returned (or threw).
    };

    /// Construct a fiber that will execute the given function when resumed.
    template <class Fn>
    explicit Fiber(Fn&& fn, std::size_t stack_size = config::kDefaultStackSize)
        : m_entry(std::forward<Fn>(fn)) {
        Init(stack_size);
    }

    JAF_API ~Fiber();

    // Fibers are pinned to memory; copying or moving is disabled to protect references to `this`.
    Fiber(const Fiber&)            = delete;
    Fiber& operator=(const Fiber&) = delete;
    Fiber(Fiber&&)                 = delete;
    Fiber& operator=(Fiber&&)      = delete;

    /// Resume execution until the fiber yields or finishes, rethrowing any exception it threw.
    JAF_API void resume();

    /// Get the current state of the fiber.
    [[nodiscard]] State state() const noexcept { return m_state; }

    /// Check if the fiber has finished executing.
    [[nodiscard]] bool  done()  const noexcept { return m_state == State::Done; }

private:
    /// Allocates the fiber stack and initializes the execution context.
    JAF_API void Init(std::size_t stack_size);

    /// Suspends the fiber and switches execution back to the resumer context.
    void DoYield() noexcept;

    /// Context trampoline entry point.
    static void Entry(detail::transfer_t t) noexcept;

    friend void this_fiber::yield() noexcept;

    void*       m_stack       = nullptr;  ///< Stack allocation base address.
    std::size_t m_stack_size  = 0;        ///< Size of the allocated stack.
    void*       m_ctx         = nullptr;  ///< Context pointer to jump into the fiber.
    void*       m_return_ctx  = nullptr;  ///< Context pointer to return to the resumer.
    State       m_state       = State::Ready;

    Task               m_entry;        ///< Type-erased entry callable task.
    std::exception_ptr m_exception;    ///< Exception captured from the entry function.
};

} // namespace jaf
