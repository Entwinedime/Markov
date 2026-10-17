#pragma once

#include "markov/performance/cost_model.hpp"
#include "markov/performance/cost_query.hpp"

#include <nlohmann/json_fwd.hpp>

#include <compare>
#include <cstdint>
#include <map>
#include <optional>

namespace markov {

class TransferCostModel final : public CostModel {
public:
    explicit TransferCostModel(const nlohmann::json & entries);

    std::optional<std::int64_t> measured_cost(const TransferCostFeatures & features) const;
    CostModelResult estimate(const CostQuery & query) const override;
    NodeCostBasis basis() const override { return NodeCostBasis::TransferCandidate; }

private:
    std::map<TransferCostFeatures, std::int64_t> entries_;
};

} // namespace markov
