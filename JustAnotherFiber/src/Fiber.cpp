// src/Fiber.cpp -- jaf::Fiber implementation over the fcontext primitives.

#include <jaf/Fiber.hpp>

#include "context/FContext.hpp"
#include "scheduler/StackAlloc.hpp"

namespace jaf {

namespace {
// The fiber currently executing on this thread (nullptr when running on the
// thread's original stack). Used by this_fiber::yield() to find its target.
thread_local Fiber* tl_active = nullptr;
} // namespace

void Fiber::Init(std::size_t stack_size) {
    JAF_ASSERT(stack_size >= config::kMinStackSize,
               "jaf::Fiber stack_size is below config::kMinStackSize");

    const detail::StackAlloc sa = detail::AllocStack(stack_size);
    m_stack      = sa.base;
    m_stack_size = sa.total;
    m_ctx = detail::make_fcontext(sa.top, sa.usable, &Fiber::Entry);
}

Fiber::~Fiber() {
    detail::FreeStack(m_stack, m_stack_size);
}

void Fiber::Entry(detail::transfer_t t) noexcept {
    auto* self = static_cast<Fiber*>(t.data);
    self->m_return_ctx = t.fctx;  // where to return to the first resumer

    try {
        self->m_entry();
    } catch (...) {
        // Exceptions must never unwind across an fcontext boundary; capture and
        // rethrow from resume() instead.
        self->m_exception = std::current_exception();
    }

    self->m_state = Fiber::State::Done;

    detail::jump_fcontext(self->m_return_ctx, nullptr);
    // unreachable
}

void Fiber::resume() {
    JAF_ASSERT(m_state != State::Running,
               "jaf::Fiber::resume() called on an already-running fiber");

    if (m_state == State::Done) {
        return;
    }

    Fiber* prev = tl_active;
    tl_active = this;
    m_state = State::Running;

    detail::transfer_t t = detail::jump_fcontext(m_ctx, this);

    // Back on the resumer's stack: the fiber has yielded or finished.
    m_ctx = t.fctx;
    tl_active = prev;

    if (m_state == State::Done && m_exception) {
        std::exception_ptr e = std::exchange(m_exception, nullptr);
        std::rethrow_exception(e);
    }
}

void Fiber::DoYield() noexcept {
    m_state = State::Suspended;

    detail::transfer_t t = detail::jump_fcontext(m_return_ctx, nullptr);
    m_return_ctx = t.fctx;
    m_state = State::Running;
}

void this_fiber::yield() noexcept {
    Fiber* self = tl_active;
    JAF_ASSERT(self != nullptr, "jaf::this_fiber::yield() called outside of a fiber");
    self->DoYield();
}

} // namespace jaf
