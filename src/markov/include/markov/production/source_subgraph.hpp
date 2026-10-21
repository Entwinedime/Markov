#pragma once

#include "markov/production/source_call.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <set>
#include <span>
#include <vector>

namespace markov {

struct TemplateDefinition;
struct TemplateConnection;

// Cuts refer to the extractor's graph and call table. They add no executable
// work and live only until the owning template source has been produced.
struct RegionSubgraph {
    std::size_t call;
    std::size_t serving_call;
    std::set<BoundaryId> inputs;
    std::set<BoundaryId> outputs;
};

struct BlockSubgraph {
    std::size_t call;
    std::vector<SourceSpanId> spans;
    std::vector<BoundaryId> logical_boundaries;
    std::vector<std::array<BoundaryId, 2>> thread_paths;
    std::vector<std::size_t> edges;
    std::map<PortKind, BoundaryId> ports;

    TemplateDefinition build(const SourceGraph & graph, std::span<const SourceCall> calls, std::span<const RegionSubgraph> regions,
                             const std::map<SourceSpanId, SourceKernel> & kernels, std::vector<TemplateConnection> connections) const;
};

struct CrossBlockEdge {
    std::size_t edge;
    std::vector<std::size_t> sources;
    std::vector<std::size_t> targets;
};

struct SourceSubgraphs {
    std::vector<BlockSubgraph> blocks;
    std::vector<RegionSubgraph> regions;
    std::vector<CrossBlockEdge> crossings;
    nlohmann::json gaps = nlohmann::json::array();
};

} // namespace markov
