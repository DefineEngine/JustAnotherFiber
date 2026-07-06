#include "scheduler/StackAlloc.hpp"

#include <cstddef>
#include <new>

#if JAF_GUARDED_STACKS
#  if !JAF_OS_WINDOWS
#    error "JAF_GUARDED_STACKS currently requires Windows (VirtualAlloc/VirtualProtect)."
#  endif
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace jaf::detail {

#if JAF_GUARDED_STACKS

namespace {
std::size_t PageSize() noexcept {
    SYSTEM_INFO si;
    ::GetSystemInfo(&si);
    return static_cast<std::size_t>(si.dwPageSize);
}
} // namespace

StackAlloc AllocStack(std::size_t usable_size) {
    const std::size_t page    = PageSize();
    const std::size_t rounded = ((usable_size + page - 1) / page) * page;
    const std::size_t total   = rounded + page;   // one guard page at the base

    void* base = ::VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    JAF_ASSERT(base != nullptr, "VirtualAlloc failed for a guarded fiber stack");

    DWORD      old = 0;
    const BOOL ok  = ::VirtualProtect(base, page, PAGE_NOACCESS, &old);
    JAF_ASSERT(ok != FALSE, "VirtualProtect(PAGE_NOACCESS) failed for the stack guard page");

    StackAlloc sa;
    sa.base   = base;
    sa.total  = total;
    sa.usable = rounded;
    sa.top    = static_cast<std::byte*>(base) + total;   // highest address; stack grows down
    return sa;
}

void FreeStack(void* base, std::size_t /*total*/) noexcept {
    if (base) ::VirtualFree(base, 0, MEM_RELEASE);
}

#else  // unguarded (release default): plain aligned allocation, no guard page.

StackAlloc AllocStack(std::size_t usable_size) {
    void* base = ::operator new(usable_size, std::align_val_t{config::kStackAlignment});
    StackAlloc sa;
    sa.base   = base;
    sa.total  = usable_size;
    sa.usable = usable_size;
    sa.top    = static_cast<std::byte*>(base) + usable_size;
    return sa;
}

void FreeStack(void* base, std::size_t /*total*/) noexcept {
    if (base) ::operator delete(base, std::align_val_t{config::kStackAlignment});
}

#endif

} // namespace jaf::detail
