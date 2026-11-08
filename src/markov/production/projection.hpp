#pragma once

#include <cstddef>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/graph/graph.hpp"
#include "markov/graph/observation.hpp"

namespace markov {

struct ProjectionEvidence {
    // Shared serial boundaries may have several anchors; one observed span supplies the same resource coordinate.
    std::vector<NativeAnchor> events;
    std::vector<std::size_t> work_spans;

    nlohmann::json document(const NativeObservation & observation) const;
};

// Source and independent target runs use this projection, without evaluating template rules.
class CoarseProjection {
public:
    static CoarseGraph project(const NativeObservation & observation, ProjectionEvidence * evidence = nullptr);
};

} // namespace markov
