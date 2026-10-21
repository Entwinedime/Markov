#pragma once

#include "markov/contracts/observation_semantics.hpp"
#include "markov/graph/source_graph.hpp"
#include "markov/production/source_subgraph.hpp"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <span>

namespace markov {

// Compares a fixed target expansion with an independent observed execution.
// Graph, calls, cuts and semantic interpretation retain their source owner.
class StructureComparison {
public:
    StructureComparison(const SourceGraph & graph, std::span<const SourceCall> calls, const SourceSubgraphs & subgraphs, const ObservationSemantics & semantics)
        : graph_(graph),
          calls_(calls),
          subgraphs_(subgraphs),
          semantics_(semantics) {}

    nlohmann::json run(const std::filesystem::path & target_graph) const;

private:
    const SourceGraph & graph_;
    std::span<const SourceCall> calls_;
    const SourceSubgraphs & subgraphs_;
    const ObservationSemantics & semantics_;
};

} // namespace markov
