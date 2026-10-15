#pragma once

// jaf/Config.hpp — the single point for platform detection, attribute wrappers,
// compile-time tunables, and the always-on assert. See docs/Architecture.md.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>

#if defined(_WIN32)
    #define JAF_OS_WINDOWS 1
#else
    #define JAF_OS_WINDOWS 0
#endif

#if defined(_MSC_VER) && !defined(__clang__)
    #define JAF_COMPILER_MSVC 1
#else
    #define JAF_COMPILER_MSVC 0
#endif

#if defined(_M_X64) || defined(__x86_64__)
    #define JAF_ARCH_X64 1
#else
    #define JAF_ARCH_X64 0
#endif

#if !JAF_OS_WINDOWS || !JAF_ARCH_X64
    #error "JustAnotherFiber currently supports Windows x64 only (see docs/Roadmap.md)."
#endif

#if JAF_COMPILER_MSVC
    #define JAF_FORCE_INLINE __forceinline
    #define JAF_NOINLINE     __declspec(noinline)
#else
    #define JAF_FORCE_INLINE inline __attribute__((always_inline))
    #define JAF_NOINLINE     __attribute__((noinline))
#endif

// Linkage of the public API. A DLL build (JAF_BUILD_SHARED) defines JAF_SHARED for
// every consumer and JAF_BUILDING for the library itself; a static build defines neither.
#if defined(JAF_SHARED)
    #if defined(JAF_BUILDING)
        #define JAF_API __declspec(dllexport)
    #else
        #define JAF_API __declspec(dllimport)
    #endif
#else
    #define JAF_API
#endif

// CPU spin-pause hint. Arch-gated (not compiler-gated) so clang-cl gets it too.
#if JAF_ARCH_X64
    #include <immintrin.h>
    #define JAF_CPU_PAUSE() _mm_pause()
#else
    #define JAF_CPU_PAUSE() ((void)0)
#endif

// Profiling hooks: no-ops unless predefined to map onto Tracy / PIX / Superluminal.
#ifndef JAF_PROFILE_JOB_BEGIN
    #define JAF_PROFILE_JOB_BEGIN(name) ((void)(name))
#endif
#ifndef JAF_PROFILE_JOB_END
    #define JAF_PROFILE_JOB_END() ((void)0)
#endif
#ifndef JAF_PROFILE_FIBER_SWITCH
    #define JAF_PROFILE_FIBER_SWITCH() ((void)0)
#endif

namespace jaf::detail {

// Reports a failed always-on assertion and aborts. source_location defaults to the
// macro expansion site, so no __FILE__/__LINE__ plumbing is needed.
[[noreturn]] inline void AssertFail(
    const char* expr, const char* msg,
    const std::source_location loc = std::source_location::current()) noexcept {
    std::fprintf(stderr, "JAF_ASSERT failed: %s\n  (%s)\n  at %s:%u\n",
                 msg, expr, loc.file_name(), static_cast<unsigned>(loc.line()));
    std::abort();
}

} // namespace jaf::detail

// Always-on fatal assertion for memory-corrupting misuse (e.g. calling
// wait_for_counter off a fiber). Unlike <cassert>, never compiled out.
#define JAF_ASSERT(cond, msg) \
    ((cond) ? (void)0 : ::jaf::detail::AssertFail(#cond, (msg)))

// Guarded fiber stacks: the lowest page is PAGE_NOACCESS so a stack overflow faults
// deterministically. On by default in debug, off in release. Override by predefining.
#ifndef JAF_GUARDED_STACKS
#  ifdef NDEBUG
#    define JAF_GUARDED_STACKS 0
#  else
#    define JAF_GUARDED_STACKS 1
#  endif
#endif

namespace jaf::config {

inline constexpr std::size_t kCacheLineSize = 64;

inline constexpr std::size_t kDefaultStackSize = 64 * 1024;

// Minimum stack a fiber may be created with; below this make_fcontext's initial
// frame would be written below the allocation.
inline constexpr std::size_t kMinStackSize = 16 * 1024;

inline constexpr std::size_t kStackAlignment = kCacheLineSize;

inline constexpr std::size_t kLargeStackSize = 512 * 1024;

static_assert(kDefaultStackSize >= kMinStackSize, "default stack below kMinStackSize");
static_assert(kLargeStackSize   >= kMinStackSize, "large stack below kMinStackSize");

inline constexpr std::size_t kDefaultSmallFibers = 128;
inline constexpr std::size_t kDefaultLargeFibers = 32;

inline constexpr unsigned kStealAttempts = 4;

// SpinLock spin iterations (with a pause hint) before yielding to the OS.
inline constexpr unsigned kSpinBeforeYield = 1024;

// Scheduling priority levels (High/Normal/Low).
inline constexpr std::size_t kPriorityLevels = 3;

// parallel_for auto-grain target: this many chunks per worker.
inline constexpr std::size_t kParallelForChunksPerWorker = 4;

// How long the pool-exhaustion watchdog waits, with every worker stalled on
// Acquire and no fiber released, before declaring a livelock and aborting.
inline constexpr auto kPoolExhaustionTimeout = std::chrono::milliseconds(500);

} // namespace jaf::config
