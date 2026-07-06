#include <jaf/Graph.hpp>

#include <jaf/Counter.hpp>
#include <jaf/Scheduler.hpp>

#include "graph/GraphImpl.hpp"
#include "scheduler/Runtime.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <span>
#include <utility>
#include <vector>

namespace jaf {

Graph::Graph() : m_impl(std::make_unique<detail::GraphImpl>()) {}
Graph::~Graph() = default;

Graph::Node Graph::Emplace(Task task) {
    const std::uint32_t idx = m_impl->AddNode(std::move(task));
    return Node{this, idx};
}

std::size_t Graph::Size() const noexcept { return m_impl->tasks.size(); }

std::pair<Graph::Node, Graph::Node> Graph::Compose(Graph& sub) {
    detail::GraphImpl* dst = m_impl.get();
    detail::GraphImpl* src = sub.m_impl.get();
    JAF_ASSERT(src != dst, "jaf::Graph::Compose cannot compose a graph into itself");
    JAF_ASSERT(!dst->in_flight.load(std::memory_order_acquire),
               "jaf::Graph::Compose while a schedule() is in flight");

    // Virtual fence nodes (empty tasks) bracket the spliced region.
    const std::uint32_t entry = dst->AddNode(Task{[] {}});
    const std::uint32_t exit  = dst->AddNode(Task{[] {}});

    const std::uint32_t base = static_cast<std::uint32_t>(dst->tasks.size());
    const std::uint32_t n    = static_cast<std::uint32_t>(src->tasks.size());

    for (std::uint32_t i = 0; i < n; ++i) {
        dst->tasks.push_back(std::move(src->tasks[i]));
        dst->stacks.push_back(src->stacks[i]);
        dst->prios.push_back(src->prios[i]);
        dst->names.push_back(src->names[i]);
        dst->succ.emplace_back();
        dst->indeg.push_back(0);
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t gi = base + i;
        if (src->indeg[i] == 0)   dst->AddEdge(entry, gi);
        if (src->succ[i].empty()) dst->AddEdge(gi, exit);
        for (std::uint32_t s : src->succ[i]) dst->AddEdge(gi, base + s);
    }

    src->tasks.clear();
    src->stacks.clear();
    src->prios.clear();
    src->names.clear();
    src->succ.clear();
    src->indeg.clear();
    src->topology_dirty = true;

    return {Node{this, entry}, Node{this, exit}};
}

void Graph::AddEdge(std::uint32_t from, std::uint32_t to) { m_impl->AddEdge(from, to); }

void Graph::SetStack(std::uint32_t index, StackClass stack_class) noexcept {
    m_impl->SetStack(index, stack_class);
}

void Graph::SetPriority(std::uint32_t index, Priority p) noexcept {
    m_impl->SetPriority(index, p);
}

void Graph::SetName(std::uint32_t index, const char* name) noexcept {
    m_impl->SetName(index, name);
}

namespace {

void GraphSubmitNode(detail::GraphImpl* g, std::uint32_t idx, Counter* c) {
    auto wrapper = [g, idx, c] {
        try {
            g->tasks[idx]();
        } catch (...) {
            detail::SpinGuard lk(g->exc_lock);
            if (!g->schedule_exception) g->schedule_exception = std::current_exception();
            return;
        }
        for (std::uint32_t s : g->succ[idx]) {
            if (g->remaining[s].fetch_sub(1, std::memory_order_acq_rel) == 1) {
                GraphSubmitNode(g, s, c);   // we released the last predecessor
            }
        }
    };
    static_assert(sizeof(wrapper) <= Task::kBufferSize,
                  "graph node wrapper must fit Task's inline buffer (no per-node heap alloc)");

    g->submitted.fetch_add(1, std::memory_order_relaxed);
    Task t = std::move(wrapper);
    jaf::run(std::span<Task>{&t, 1}, c, g->stacks[idx], g->prios[idx], g->names[idx]);
}

bool HasCycle(const detail::GraphImpl& g) {
    const std::size_t          n = g.tasks.size();
    std::vector<std::uint32_t> indeg(g.indeg);
    std::vector<std::uint32_t> ready;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (indeg[i] == 0) ready.push_back(i);
    }
    std::size_t processed = 0;
    while (!ready.empty()) {
        const std::uint32_t u = ready.back();
        ready.pop_back();
        ++processed;
        for (std::uint32_t v : g.succ[u]) {
            if (--indeg[v] == 0) ready.push_back(v);
        }
    }
    return processed != n;
}

void FinishRun(detail::GraphImpl* impl) {
    struct Clear {
        detail::GraphImpl* i;
        ~Clear() { i->in_flight.store(false, std::memory_order_release); }
    } clear{impl};

    // Rethrow this run's own exception before the completeness check: pruning
    // means fewer than Size() nodes ran, which is expected on failure.
    if (impl->schedule_exception) {
        std::exception_ptr e = impl->schedule_exception;
        impl->schedule_exception = nullptr;
        std::rethrow_exception(e);
    }

    JAF_ASSERT(impl->submitted.load(std::memory_order_relaxed) ==
                   static_cast<std::uint32_t>(impl->tasks.size()),
               "jaf::schedule: graph did not fully execute (cyclic or unreachable nodes)");
}

} // namespace

GraphRun schedule_async(Graph& graph) {
    detail::GraphImpl* impl = graph.m_impl.get();
    if (impl->tasks.empty()) return GraphRun{};   // nothing to run -> null handle

    JAF_ASSERT(!impl->in_flight.exchange(true, std::memory_order_acquire),
               "jaf::schedule_async re-entered or graph mutated while a run is in flight");

    if (impl->topology_dirty) {
        JAF_ASSERT(!HasCycle(*impl),
                   "jaf::schedule_async given a graph with a cycle (a DAG is required)");
        impl->topology_dirty = false;
    }

    impl->ResetRunState();

    const auto n = static_cast<std::uint32_t>(impl->tasks.size());
    for (std::uint32_t i = 0; i < n; ++i) {
        if (impl->indeg[i] == 0) {
            GraphSubmitNode(impl, i, &impl->run_counter);
        }
    }
    return GraphRun{impl};
}

void schedule(Graph& graph) {
    JAF_ASSERT(detail::CurrentFiber() != nullptr,
               "jaf::schedule must be called from inside a fiber/job");
    schedule_async(graph).wait();
}

GraphRun::GraphRun(GraphRun&& other) noexcept : m_impl(other.m_impl) {
    other.m_impl = nullptr;
}

GraphRun& GraphRun::operator=(GraphRun&& other) noexcept {
    if (this != &other) {
        JAF_ASSERT(m_impl == nullptr, "GraphRun overwritten while a run is still in flight");
        m_impl       = other.m_impl;
        other.m_impl = nullptr;
    }
    return *this;
}

GraphRun::~GraphRun() {
    JAF_ASSERT(m_impl == nullptr, "GraphRun destroyed without a completed wait()");
}

void GraphRun::wait() {
    if (m_impl == nullptr) return;
    detail::GraphImpl* impl = m_impl;
    m_impl = nullptr;   // consume the handle
    if (detail::CurrentFiber() != nullptr) {
        jaf::wait_for_counter(&impl->run_counter, 0);
    } else {
        jaf::wait_for_counter_blocking(&impl->run_counter, 0);
    }
    FinishRun(impl);    // may rethrow; always clears in_flight
}

bool GraphRun::wait_until(std::chrono::steady_clock::time_point deadline) {
    if (m_impl == nullptr) return true;
    detail::GraphImpl* impl = m_impl;
    const bool completed = (detail::CurrentFiber() != nullptr)
        ? jaf::wait_for_counter_until(&impl->run_counter, 0, deadline)
        : jaf::wait_for_counter_blocking_until(&impl->run_counter, 0, deadline);
    if (!completed) return false;   // still live: keep the handle and in_flight
    m_impl = nullptr;               // consume the handle
    FinishRun(impl);                // may rethrow; always clears in_flight
    return true;
}

bool GraphRun::done() const noexcept {
    return m_impl == nullptr || jaf::try_wait_for_counter(&m_impl->run_counter, 0);
}

Counter& GraphRun::await_counter() const noexcept { return m_impl->run_counter; }

void GraphRun::finalize_completed() {
    detail::GraphImpl* impl = m_impl;
    m_impl = nullptr;   // consume the handle
    FinishRun(impl);
}

} // namespace jaf
