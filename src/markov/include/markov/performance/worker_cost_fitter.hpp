#pragma once

#include "markov/graph/source_graph.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <span>
#include <string>

namespace markov {

struct WorkerCostSample {
    std::string operation;
    CpuSubmissionPart part;
    std::int64_t observed_ns;
};

// An operation-level CPU hypothesis, not an assumption that all shapes have
// identical cost. Its transfer error must be evaluated without refitting.
class WorkerCostFitter {
public:
    nlohmann::json fit(std::span<const WorkerCostSample> samples) const;
    nlohmann::json evaluate(const nlohmann::json & entries, std::span<const WorkerCostSample> samples) const;
};

} // namespace markov
