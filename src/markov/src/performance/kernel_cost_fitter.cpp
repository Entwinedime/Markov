#include "markov/performance/kernel_cost_fitter.hpp"

#include "markov/performance/cost_statistics.hpp"
#include "markov/performance/kernel_cost_model.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <utility>

namespace markov {

nlohmann::json KernelCostFitter::fit(std::span<const KernelCostSample> samples) const {
    std::map<KernelCostFeatures, CostDistribution> groups;
    for (const auto & sample : samples) groups[sample.features].observe(sample.observed_ns);

    auto entries = nlohmann::json::array();
    for (const auto & [features, distribution] : groups) {
        auto entry = distribution.describe();
        entry["features"] = features.describe();
        entries.push_back(std::move(entry));
    }
    return entries;
}

nlohmann::json KernelCostFitter::evaluate(const nlohmann::json & entries, std::span<const KernelCostSample> samples) const {
    const KernelCostModel estimates(entries);

    CostErrors total;
    std::map<KernelCostFeatures, CostErrors> groups;
    std::map<KernelCostFeatures, std::size_t> unmatched;
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

} // namespace markov
