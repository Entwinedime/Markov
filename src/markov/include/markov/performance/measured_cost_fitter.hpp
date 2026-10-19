#pragma once

#include "markov/performance/cost_statistics.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <utility>

namespace markov {

// Own each feature definition once. Fitting and evaluation consume the same
// measured groups without reconstructing a per-sample feature table.
template <class Features> class MeasuredCostFitter {
public:
    void observe(const Features & features, std::int64_t duration_ns) { groups_[features].observe(duration_ns); }

    nlohmann::json fit() const {
        auto entries = nlohmann::json::array();
        for (const auto & [features, distribution] : groups_) {
            auto entry = distribution.describe();
            entry["features"] = features.describe();
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    template <class Model> nlohmann::json evaluate(const Model & estimates) const {
        CostErrors total;
        auto reports = nlohmann::json::array();
        auto missing = nlohmann::json::array();
        for (const auto & [features, distribution] : groups_) {
            const auto estimate = estimates.measured_cost(features);
            if (!estimate) {
                missing.push_back({
                    { "features",           features.describe() },
                    {    "count", distribution.samples().size() }
                });
                continue;
            }

            CostErrors errors;
            for (const auto observed_ns : distribution.samples()) {
                total.observe(*estimate, observed_ns);
                errors.observe(*estimate, observed_ns);
            }
            reports.push_back({
                { "features", features.describe() },
                {   "errors",   errors.describe() }
            });
        }

        return {
            {   "overall",   total.describe() },
            {    "groups", std::move(reports) },
            { "unmatched", std::move(missing) }
        };
    }

private:
    std::map<Features, CostDistribution> groups_;
};

} // namespace markov
