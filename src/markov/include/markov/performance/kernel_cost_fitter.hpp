#pragma once

#include "markov/performance/kernel_cost_model.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <span>

namespace markov {

struct KernelCostSample {
    KernelCostFeatures features;
    std::int64_t observed_ns;
};

class KernelCostFitter {
public:
    nlohmann::json fit(std::span<const KernelCostSample> samples) const;
    nlohmann::json evaluate(const nlohmann::json & entries, std::span<const KernelCostSample> samples) const;
};

} // namespace markov
