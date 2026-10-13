#pragma once

#include "markov/performance/cost_model.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace markov {

enum class CpuCostUnit { Admission, Worker };

// Immutable empirical values. File/context ownership and applicability belong
// to the workflow; this class matches the actual work of one CPU service.
class CpuCostModel final : public CostModel {
public:
    CpuCostModel(CpuCostUnit unit, const nlohmann::json & entries);

    std::optional<std::int64_t> admission_cost(const AdmissionServiceWork & work) const;
    std::optional<std::int64_t> worker_cost(const std::string & operation, CpuSubmissionPart part) const;
    std::optional<std::int64_t> estimate(const ServiceWork & work) const override;
    NodeCostBasis basis() const override { return unit_ == CpuCostUnit::Admission ? NodeCostBasis::AdmissionCandidate : NodeCostBasis::WorkerCandidate; }

private:
    CpuCostUnit unit_;
    std::map<std::tuple<std::size_t, std::size_t, std::size_t>, std::int64_t> admission_;
    std::map<std::pair<std::string, CpuSubmissionPart>, std::int64_t> workers_;
};

} // namespace markov
