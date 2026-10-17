#pragma once

#include "markov/performance/cost_model.hpp"
#include "markov/performance/cost_query.hpp"

#include <nlohmann/json_fwd.hpp>

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace markov {

class CommunicationCostModel final : public CostModel {
public:
    explicit CommunicationCostModel(const nlohmann::json & entries);

    std::optional<std::int64_t> measured_cost(const CommunicationCostFeatures & features) const;
    CostModelResult estimate(const CostQuery & query) const override;
    NodeCostBasis basis() const override { return NodeCostBasis::CommunicationCandidate; }

private:
    std::map<CommunicationCostFeatures, std::int64_t> entries_;
};

} // namespace markov
