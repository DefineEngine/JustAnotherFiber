#pragma once

// jaf/Graph.hpp -- the L3 DAG task API: build a graph of tasks with precedence
// edges, then run the whole DAG with jaf::schedule(g) from inside a fiber.

#include <jaf/Config.hpp>
#include <jaf/Scheduler.hpp>   // for jaf::StackClass
#include <jaf/Task.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace jaf {

class Counter;
class GraphRun;
namespace detail { struct GraphImpl; }

/// A directed-acyclic graph of tasks lowered onto the L2 counter runtime.
class Graph {
public:
    /// Construct an empty graph.
    Graph();
    ~Graph();

    Graph(const Graph&)            = delete;
    Graph& operator=(const Graph&) = delete;
    Graph(Graph&&)                 = delete;
    Graph& operator=(Graph&&)      = delete;

    /// A lightweight handle to a node in a Graph.
    class Node {
    public:
        Node() = default;

        /// Add precedence edges: this node runs before every given node.
        template <class... Ns>
        Node Precede(Ns... succs) {
            static_assert((std::is_same_v<Ns, Node> && ...),
                          "Graph::Node::Precede() takes Graph::Node handles");
            (LinkBefore(succs), ...);
            return *this;
        }

        /// Add precedence edges: this node runs after every given node.
        template <class... Ns>
        Node Succeed(Ns... preds) {
            static_assert((std::is_same_v<Ns, Node> && ...),
                          "Graph::Node::Succeed() takes Graph::Node handles");
            (preds.LinkBefore(*this), ...);
            return *this;
        }

        /// Select the fiber stack-size class this node's task runs on.
        Node Stack(StackClass stack_class);

        /// Select the scheduling priority for this node's task.
        Node Prio(Priority p);

        /// Give this node a profiler zone name (stored by pointer).
        Node Name(const char* name);

        /// True if this handle refers to a node (not a default-constructed null).
        [[nodiscard]] explicit operator bool() const noexcept { return m_graph != nullptr; }

    private:
        friend class Graph;
        Node(Graph* g, std::uint32_t idx) noexcept : m_graph(g), m_index(idx) {}

        void LinkBefore(Node succ) const;

        Graph*        m_graph = nullptr;
        std::uint32_t m_index = 0;
    };

    /// Add a task to the graph as a new node.
    Node Emplace(Task task);

    /// Splice `sub` into this graph as a fenced region; returns {entry, exit}
    /// handles bracketing the spliced region. `sub` is consumed.
    std::pair<Node, Node> Compose(Graph& sub);

    /// Number of nodes currently in the graph.
    [[nodiscard]] std::size_t Size() const noexcept;

private:
    friend class Node;
    friend void schedule(Graph&);
    friend GraphRun schedule_async(Graph&);

    void AddEdge(std::uint32_t from, std::uint32_t to);

    void SetStack(std::uint32_t index, StackClass stack_class) noexcept;

    void SetPriority(std::uint32_t index, Priority p) noexcept;

    void SetName(std::uint32_t index, const char* name) noexcept;

    std::unique_ptr<detail::GraphImpl> m_impl;
};

// ---- Node fluent helpers (defined once Graph is complete) -------------------

inline void Graph::Node::LinkBefore(Node succ) const {
    JAF_ASSERT(m_graph != nullptr && succ.m_graph == m_graph,
               "Graph::Node edge endpoints must be non-null handles from the same Graph");
    m_graph->AddEdge(m_index, succ.m_index);
}

inline Graph::Node Graph::Node::Stack(StackClass stack_class) {
    JAF_ASSERT(m_graph != nullptr, "Graph::Node::Stack() on a null handle");
    m_graph->SetStack(m_index, stack_class);
    return *this;
}

inline Graph::Node Graph::Node::Prio(Priority p) {
    JAF_ASSERT(m_graph != nullptr, "Graph::Node::Prio() on a null handle");
    m_graph->SetPriority(m_index, p);
    return *this;
}

inline Graph::Node Graph::Node::Name(const char* name) {
    JAF_ASSERT(m_graph != nullptr, "Graph::Node::Name() on a null handle");
    m_graph->SetName(m_index, name);
    return *this;
}

/// Run an entire DAG to completion. Parks the calling fiber until the graph
/// drains; must be called from inside a fiber/job. Rethrows the first node
/// exception from Scheduler::run.
void schedule(Graph& graph);

/// A handle to an in-flight asynchronous graph run.
class GraphRun {
public:
    GraphRun() = default;
    GraphRun(GraphRun&& other) noexcept;
    GraphRun& operator=(GraphRun&& other) noexcept;
    GraphRun(const GraphRun&)            = delete;
    GraphRun& operator=(const GraphRun&) = delete;
    ~GraphRun();

    /// Join the run to completion, rethrowing the first node exception; consumes
    /// the handle. Parks if called from a fiber, blocks the OS thread otherwise.
    void wait();

    /// Join with a deadline. Returns true if the graph completed (then rethrows
    /// any node exception and consumes the handle); false on timeout.
    bool wait_until(std::chrono::steady_clock::time_point deadline);

    /// Join with a relative timeout. See wait_until.
    template <class Rep, class Period>
    bool wait_for(std::chrono::duration<Rep, Period> timeout) {
        return wait_until(std::chrono::steady_clock::now() + timeout);
    }

    /// Non-blocking check: true if the run has already drained (a hint).
    [[nodiscard]] bool done() const noexcept;

    /// True if this handle refers to a live run.
    [[nodiscard]] explicit operator bool() const noexcept { return m_impl != nullptr; }

    // ---- coroutine support (jaf/Coro.hpp) -- not for direct use ----
    /// The run's join counter, for a coroutine awaiter to register on.
    [[nodiscard]] Counter& await_counter() const noexcept;
    /// Consume the handle and finalize an already-drained run.
    void finalize_completed();

private:
    friend GraphRun schedule_async(Graph&);
    explicit GraphRun(detail::GraphImpl* impl) noexcept : m_impl(impl) {}

    detail::GraphImpl* m_impl = nullptr;
};

/// Submit a DAG and return immediately with a join handle. Callable from a fiber
/// or a plain thread; join with GraphRun::wait().
[[nodiscard]] GraphRun schedule_async(Graph& graph);

} // namespace jaf
