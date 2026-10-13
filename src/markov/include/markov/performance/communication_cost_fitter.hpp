#pragma once

#include "markov/performance/communication_cost_model.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <span>

namespace markov {

struct CommunicationCostSample {
    CommunicationCostFeatures features;
    std::int64_t observed_ns;
};

class CommunicationCostFitter {
public:
    nlohmann::json fit(std::span<const CommunicationCostSample> samples) const;
    nlohmann::json evaluate(const nlohmann::json & entries, std::span<const CommunicationCostSample> samples) const;
};

} // namespace markov
