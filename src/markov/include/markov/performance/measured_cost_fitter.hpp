#pragma once

#include "markov/performance/cost_statistics.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <span>
#include <utility>

namespace markov {

template <class Features> struct MeasuredCostSample {
    Features features;
    std::int64_t observed_ns;
};

// Independently measured candidates share exact-key statistics. Their models
// retain ownership of feature interpretation and target-work applicability.
template <class Features> class MeasuredCostFitter {
public:
    nlohmann::json fit(std::span<const MeasuredCostSample<Features>> samples) const {
        std::map<Features, CostDistribution> groups;
        for (const auto & sample : samples) groups[sample.features].observe(sample.observed_ns);

        auto entries = nlohmann::json::array();
        for (const auto & [features, distribution] : groups) {
            auto entry = distribution.describe();
            entry["features"] = features.describe();
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    template <class Model> nlohmann::json evaluate(const Model & estimates, std::span<const MeasuredCostSample<Features>> samples) const {
        CostErrors total;
        std::map<Features, CostErrors> groups;
        std::map<Features, std::size_t> unmatched;
        for (const auto & sample : samples) {
            const auto estimate = estimates.measured_cost(sample.features);
            if (!estimate) {
                ++unmatched[sample.features];
                continue;
            }
            total.observe(*estimate, sample.observed_ns);
            groups[sample.features].observe(*estimate, sample.observed_ns);
        }

        auto reports = nlohmann::json::array();
        for (const auto & [features, errors] : groups)
            reports.push_back({
                { "features", features.describe() },
                {   "errors",   errors.describe() }
            });

        auto missing = nlohmann::json::array();
        for (const auto & [features, count] : unmatched)
            missing.push_back({
                { "features", features.describe() },
                {    "count",               count }
            });

        return {
            {   "overall",   total.describe() },
            {    "groups", std::move(reports) },
            { "unmatched", std::move(missing) }
        };
    }
};

} // namespace markov
