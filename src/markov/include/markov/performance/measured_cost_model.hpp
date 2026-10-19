#pragma once

#include "markov/contracts/json_input.hpp"
#include "markov/performance/cost_model.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>

namespace markov {

// Exact measured tables share storage and lookup. Feature interpretation stays
// on the work contract; specialized models retain their own query policies.
template <class Features, CostQueryKind Kind, NodeCostBasis Basis> class MeasuredCostModel final : public CostModel {
public:
    explicit MeasuredCostModel(const nlohmann::json & entries) {
        for (const auto & entry : entries) {
            const auto features = Features::read(entry.at("features"));
            const auto duration = input::integer(entry.at("estimate_ns"), 0, std::numeric_limits<std::int64_t>::max());
            if (!entries_.emplace(features, duration).second) throw std::invalid_argument("duplicate measured cost features");
        }
    }

    std::optional<std::int64_t> measured_cost(const Features & features) const {
        const auto found = entries_.find(features);
        if (found == entries_.end()) return std::nullopt;
        return found->second;
    }

    CostModelResult estimate(const CostQuery & query) const override {
        if (query.kind == Kind && query.issue == CostQueryIssue::None) return measured(measured_cost(std::get<Features>(query.features)));
        return std::unexpected(CostModelGap{});
    }

    NodeCostBasis basis() const override { return Basis; }

private:
    std::map<Features, std::int64_t> entries_;
};

} // namespace markov
