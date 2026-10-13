#include "markov/performance/communication_cost_fitter.hpp"

#include "markov/performance/cost_statistics.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <utility>

namespace markov {

nlohmann::json CommunicationCostFitter::fit(std::span<const CommunicationCostSample> samples) const {
    std::map<CommunicationCostFeatures, CostDistribution> groups;
    for (const auto & sample : samples) groups[sample.features].observe(sample.observed_ns);

    auto entries = nlohmann::json::array();
    for (const auto & [features, distribution] : groups) {
        auto entry = distribution.describe();
        entry["features"] = features.describe();
        entries.push_back(std::move(entry));
    }
    return entries;
}

nlohmann::json CommunicationCostFitter::evaluate(const nlohmann::json & entries, std::span<const CommunicationCostSample> samples) const {
    const CommunicationCostModel estimates(entries);
    CostErrors total;
    std::map<CommunicationCostFeatures, CostErrors> groups;
    std::map<CommunicationCostFeatures, std::size_t> unmatched;
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
