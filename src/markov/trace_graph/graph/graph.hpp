#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace markov::trace_graph::graph {

using NodeId = std::size_t;
using Time = std::int64_t;

enum class NodeKind { Service, ObservedWait, ProfilerOverhead, ResidualGap, Boundary, Delay };
enum class EdgeKind { ResourceOrder, ObservedFlow, ClientOrder, Submission, Synchronization, DataDependency };

struct Node {
    Time duration_ns;
    std::size_t resource;
    NodeKind kind;
};

struct Edge {
    NodeId from;
    NodeId to;
    EdgeKind kind;
};

void canonicalize_edges(std::vector<Edge> & edges);

// Concrete execution costs and resources only; source clocks and provenance
// stay outside the core graph. Finalize before reading indexed dependencies.
class Graph {
public:
    NodeId append(Node node);
    void connect(NodeId from, NodeId to, EdgeKind kind);
    void finalize();

    std::span<const Node> nodes() const { return nodes_; }
    std::span<const Edge> edges() const { return edges_; }
    std::span<const Edge> successors(NodeId node) const;
    std::span<const std::size_t> incoming_counts() const { return incoming_; }

private:
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::vector<std::size_t> offsets_;
    std::vector<std::size_t> incoming_;
};

} // namespace markov::trace_graph::graph
