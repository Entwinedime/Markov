#pragma once

#include "markov/performance/service_work.hpp"

#include <cstdint>
#include <optional>

namespace markov {

enum class NodeCostBasis { Given, AdmissionCandidate, WorkerCandidate, KernelCandidate, CommunicationCandidate };

// Models consume target work through the same query. The workflow owns their
// deployment applicability and lifetime; queries never collect or refit.
class CostModel {
public:
    virtual ~CostModel() = default;
    virtual std::optional<std::int64_t> estimate(const ServiceWork & work) const = 0;
    virtual NodeCostBasis basis() const = 0;
};

} // namespace markov
