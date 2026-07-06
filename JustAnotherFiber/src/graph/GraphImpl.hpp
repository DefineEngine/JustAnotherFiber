#pragma once

// graph/GraphImpl.hpp -- structure-of-arrays node arena behind jaf::Graph.

#include <jaf/Counter.hpp>
#include <jaf/Scheduler.hpp>
#include <jaf/Task.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <vector>

namespace jaf::detail {

struct GraphImpl {
    std::vector<Task>                       tasks;
    std::vector<StackClass>                 stacks;
    std::vector<Priority>                   prios;
    std::vector<const char*>                names;
    std::vector<std::vector<std::uint32_t>> succ;
    std::vector<std::uint32_t>              indeg;
    bool                                    topology_dirty = true;

    std::unique_ptr<std::atomic<int>[]>     remaining;
    std::size_t                             remaining_cap = 0;
    std::atomic<std::uint32_t>              submitted{0};
    std::atomic<bool>                       in_flight{false};
    SpinLock                                exc_lock;
    std::exception_ptr                      schedule_exception;
    Counter                                 run_counter;

    /// Append a node and return its index.
    std::uint32_t AddNode(Task task) {
        JAF_ASSERT(!in_flight.load(std::memory_order_acquire),
                   "jaf::Graph mutated while a schedule() is in flight");
        topology_dirty = true;
        const auto idx = static_cast<std::uint32_t>(tasks.size());
        tasks.push_back(std::move(task));
        stacks.push_back(StackClass::Small);
        prios.push_back(Priority::Normal);
        names.push_back(nullptr);
        succ.emplace_back();
        indeg.push_back(0);
        return idx;
    }

    /// Record a precedence edge `from` -> `to` and bump `to`'s in-degree.
    void AddEdge(std::uint32_t from, std::uint32_t to) {
        JAF_ASSERT(!in_flight.load(std::memory_order_acquire),
                   "jaf::Graph edge added while a schedule() is in flight");
        topology_dirty = true;
        succ[from].push_back(to);
        ++indeg[to];
    }

    /// Set the fiber stack class for a node.
    void SetStack(std::uint32_t idx, StackClass cls) noexcept {
        JAF_ASSERT(!in_flight.load(std::memory_order_acquire),
                   "jaf::Graph node stack changed while a schedule() is in flight");
        stacks[idx] = cls;
    }

    /// Set the scheduling priority for a node.
    void SetPriority(std::uint32_t idx, Priority p) noexcept {
        JAF_ASSERT(!in_flight.load(std::memory_order_acquire),
                   "jaf::Graph node priority changed while a schedule() is in flight");
        prios[idx] = p;
    }

    /// Set the profiler name for a node (borrowed pointer).
    void SetName(std::uint32_t idx, const char* n) noexcept {
        JAF_ASSERT(!in_flight.load(std::memory_order_acquire),
                   "jaf::Graph node named while a schedule() is in flight");
        names[idx] = n;
    }

    /// (Re)initialise per-schedule run state for a fresh schedule().
    void ResetRunState() {
        submitted.store(0, std::memory_order_relaxed);
        schedule_exception = nullptr;
        CounterReset(run_counter);
        const std::size_t n = tasks.size();
        if (n == 0) return;
        if (n > remaining_cap) {
            remaining     = std::make_unique<std::atomic<int>[]>(n);
            remaining_cap = n;
        }
        for (std::size_t i = 0; i < n; ++i) {
            remaining[i].store(static_cast<int>(indeg[i]), std::memory_order_relaxed);
        }
    }
};

} // namespace jaf::detail
