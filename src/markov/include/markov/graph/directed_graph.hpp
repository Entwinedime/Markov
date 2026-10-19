#pragma once

#include <cstddef>
#include <numeric>
#include <span>
#include <vector>

namespace markov {

// Compact adjacency shared by source validation and target scheduling. Edge
// indices retain the caller's payload; the graph owns only traversal state.
class DirectedGraph {
public:
    template <class Edge> DirectedGraph(std::size_t vertices, const std::vector<Edge> & edges) : offsets_(vertices + 1), edges_(edges.size()) {
        std::vector<std::size_t> incoming(vertices);
        for (const auto & edge : edges) {
            ++offsets_[edge.source + 1];
            ++incoming[edge.target];
        }
        std::partial_sum(offsets_.begin(), offsets_.end(), offsets_.begin());
        auto cursor = offsets_;
        for (std::size_t index = 0; index < edges.size(); ++index) edges_[cursor[edges[index].source]++] = index;

        for (std::size_t vertex = 0; vertex < vertices; ++vertex)
            if (incoming[vertex] == 0) order_.push_back(vertex);

        for (std::size_t next = 0; next < order_.size(); ++next)
            for (const auto index : outgoing(order_[next]))
                if (--incoming[edges[index].target] == 0) order_.push_back(edges[index].target);
    }

    bool acyclic() const { return order_.size() + 1 == offsets_.size(); }
    std::span<const std::size_t> order() const { return order_; }
    std::span<const std::size_t> outgoing(std::size_t vertex) const {
        return std::span(edges_).subspan(offsets_[vertex], offsets_[vertex + 1] - offsets_[vertex]);
    }

private:
    std::vector<std::size_t> offsets_;
    std::vector<std::size_t> edges_;
    std::vector<std::size_t> order_;
};

} // namespace markov
