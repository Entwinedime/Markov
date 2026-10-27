#include "markov/trace_graph/graph/graph.hpp"

#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace markov::trace_graph::graph {

NodeId Graph::append(Node node) {
    nodes_.push_back(std::move(node));
    return nodes_.size() - 1;
}

void Graph::connect(NodeId from, NodeId to, EdgeKind kind) { edges_.push_back({ from, to, kind }); }

void canonicalize_edges(std::vector<Edge> & edges) {
    std::ranges::sort(edges, [](const Edge & a, const Edge & b) { return std::tie(a.from, a.to, a.kind) < std::tie(b.from, b.to, b.kind); });
    const auto duplicates = std::ranges::unique(edges, [](const Edge & a, const Edge & b) { return a.from == b.from && a.to == b.to && a.kind == b.kind; });
    edges.erase(duplicates.begin(), duplicates.end());
}

void Graph::finalize() {
    canonicalize_edges(edges_);
    offsets_.assign(nodes_.size() + 1, 0);
    incoming_.assign(nodes_.size(), 0);
    for (const auto & edge : edges_) {
        if (edge.from >= nodes_.size() || edge.to >= nodes_.size()) throw std::runtime_error("DAG edge endpoint is unavailable");
        ++offsets_[edge.from + 1];
        ++incoming_[edge.to];
    }
    for (std::size_t i = 1; i < offsets_.size(); ++i) offsets_[i] += offsets_[i - 1];
}

std::span<const Edge> Graph::successors(NodeId node) const { return std::span(edges_).subspan(offsets_[node], offsets_[node + 1] - offsets_[node]); }

} // namespace markov::trace_graph::graph
