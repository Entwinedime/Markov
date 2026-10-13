#pragma once

#include "markov/performance/cost_model.hpp"

#include <nlohmann/json_fwd.hpp>

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace markov {

// One physical task, not a collective envelope. Rank endpoints distinguish
// routes within the recorded deployment; task IDs are only provenance.
struct CommunicationCostFeatures {
    SourceSpanKind kind;
    std::string operation;
    std::uint64_t bytes;
    std::string data_type;
    std::string transport;
    std::uint32_t source_rank;
    std::uint32_t destination_rank;

    auto operator<=>(const CommunicationCostFeatures &) const = default;
    static CommunicationCostFeatures read(const nlohmann::json & data);
    nlohmann::json describe() const;
};

class CommunicationCostModel final : public CostModel {
public:
    explicit CommunicationCostModel(const nlohmann::json & entries);

    std::optional<std::int64_t> measured_cost(const CommunicationCostFeatures & features) const;
    std::optional<std::int64_t> estimate(const ServiceWork & work) const override;
    NodeCostBasis basis() const override { return NodeCostBasis::CommunicationCandidate; }

private:
    std::map<CommunicationCostFeatures, std::int64_t> entries_;
};

} // namespace markov
