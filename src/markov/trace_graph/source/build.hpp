#pragma once

#include "markov/trace_graph/graph/graph.hpp"
#include "markov/trace_graph/source/observations.hpp"

#include <map>
#include <vector>

namespace markov::trace_graph::source {

struct SourceGraph {
    struct NodeOrigin {
        Time begin_ns;
        Index span;
    };
    graph::Graph graph;
    std::vector<NodeOrigin> node_origins;
    std::vector<graph::NodeId> point_nodes;
    std::map<std::string, std::size_t> audit;
    struct DependencyTimingIssue {
        Index before;
        Index after;
        Time delta_ns;
    };
    Time max_dependency_clock_violation_ns = 0;
    std::vector<DependencyTimingIssue> dependency_timing_issues;
    struct HttpEndpoint {
        RequestWindow request;
        graph::NodeId start;
        graph::NodeId ingress;
        graph::NodeId completion;
    };
    std::vector<HttpEndpoint> requests;

    graph::NodeId append(Time begin_ns, Time duration_ns, Index resource, Index span, graph::NodeKind kind);
};

// Partition observed CPU wall intervals without charging both parent and child.
// Their waiting portions are not yet proof of transferable CPU service costs.
// Preserve named boundaries and explicit flow edges; retain CPU gaps separately.
SourceGraph build_source_graph(const Observations & observations);

} // namespace markov::trace_graph::source
