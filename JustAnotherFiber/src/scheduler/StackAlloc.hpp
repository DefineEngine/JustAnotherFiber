#pragma once

// scheduler/StackAlloc.hpp -- the single fiber-stack allocator, so guarding is
// decided in one place (config::JAF_GUARDED_STACKS).

#include <jaf/Config.hpp>

#include <cstddef>

namespace jaf::detail {

/// @brief A fiber-stack allocation. base/total identify it for FreeStack;
///        top/usable are what make_fcontext consumes.
struct StackAlloc {
    void*       base   = nullptr;  ///< Allocation base (lowest address); pass to FreeStack.
    std::size_t total  = 0;        ///< Total allocation size (incl. any guard page); pass to FreeStack.
    void*       top    = nullptr;  ///< Highest address of the usable region (for make_fcontext).
    std::size_t usable = 0;        ///< Usable stack bytes (for make_fcontext).
};

/// @brief Allocate a fiber stack with at least `usable_size` usable bytes.
///        Guarded builds add a PAGE_NOACCESS page below the usable region.
StackAlloc AllocStack(std::size_t usable_size);

/// @brief Release a stack previously returned by AllocStack. base==nullptr is a no-op.
void FreeStack(void* base, std::size_t total) noexcept;

} // namespace jaf::detail
