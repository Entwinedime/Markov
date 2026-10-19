#pragma once

#include "markov/production/source_subgraph.hpp"

#include <span>
#include <vector>

namespace markov {

class SourceBindings;

// The source extractor owns these observations for the complete extraction call.
// Framework adapters resolve native ports; ownership and graph assembly are shared.
class BlockExtractor {
public:
    BlockExtractor(const SourceGraph & graph, std::span<const SourceCall> calls, const SourceBindings & bindings);

    SourceSubgraphs extract() const;

private:
    const SourceGraph & graph_;
    std::span<const SourceCall> calls_;

    std::vector<RegionSubgraph> extract_regions() const;
    const SourceBindings & bindings_;
};


} // namespace markov
