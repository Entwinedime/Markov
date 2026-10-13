#pragma once

#include "markov/performance/service_work.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <span>

namespace markov {

struct AdmissionCostSample {
    AdmissionServiceWork work;
    std::int64_t observed_ns;
};

// Fits a candidate distribution; qualification and deployment stay with its asset.
class AdmissionCostFitter {
public:
    nlohmann::json fit(std::span<const AdmissionCostSample> samples) const;
    nlohmann::json evaluate(const nlohmann::json & entries, std::span<const AdmissionCostSample> samples) const;
};

} // namespace markov
