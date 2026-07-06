#pragma once

// scheduler/FiberPool.hpp -- owns all PooledFibers (and their stacks),
// pre-allocated in two size classes and handed out via intrusive free-lists.

#include "scheduler/PooledFiber.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace jaf::detail {

/// @brief Allocates and pools recyclable execution stacks (PooledFibers).
class FiberPool {
public:
    /// @brief Construct the pool, pre-allocating fibers.
    FiberPool(std::size_t small_count, std::size_t large_count);
    ~FiberPool() = default;

    FiberPool(const FiberPool&)            = delete;
    FiberPool& operator=(const FiberPool&) = delete;

    /// @brief Acquire a free fiber of the specified stack class.
    /// @return A pointer to the fiber, or nullptr if the pool class is exhausted.
    ///         A Small request may borrow a free Large fiber (never the reverse).
    PooledFiber* Acquire(StackClass cls) noexcept;

    /// @brief Release a finished fiber back to the free list.
    void         Release(PooledFiber* f) noexcept;

    /// @brief Monotonically increases on every Release, so a watchdog can tell
    ///        "the pool made progress" from "nothing released".
    std::uint64_t ReleaseGeneration() const noexcept {
        return m_release_gen.load(std::memory_order_relaxed);
    }

    /// @brief Occupancy of one stack class (total fibers, and how many are in use).
    struct ClassOccupancy { std::size_t total = 0; std::size_t in_use = 0; };
    ClassOccupancy Occupancy(StackClass cls) noexcept;

    /// @brief Format a fatal pool-exhaustion diagnostic and abort.
    void ReportExhaustion(StackClass cls) noexcept;

private:
    /// @brief Create a new fiber execution context.
    PooledFiber* MakeFiber(StackClass cls);

    std::vector<std::unique_ptr<PooledFiber>> m_storage; ///< Storage container for all fibers in the pool.
    std::mutex   m_mutex;                                ///< Mutex protecting both small and large free-lists.
    PooledFiber* m_small_free = nullptr;                 ///< Head of the small free fiber list.
    PooledFiber* m_large_free = nullptr;                 ///< Head of the large free fiber list.

    // Occupancy bookkeeping (all guarded by m_mutex except m_release_gen).
    std::size_t m_small_total      = 0;                  ///< Total Small fibers created.
    std::size_t m_large_total      = 0;                  ///< Total Large fibers created.
    std::size_t m_small_free_count = 0;                  ///< Small fibers currently free.
    std::size_t m_large_free_count = 0;                  ///< Large fibers currently free.
    std::atomic<std::uint64_t> m_release_gen{0};         ///< Bumped on every Release.
};

} // namespace jaf::detail
