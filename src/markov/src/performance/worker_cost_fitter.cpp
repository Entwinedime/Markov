#include "markov/performance/worker_cost_fitter.hpp"

#include "markov/performance/cost_statistics.hpp"
#include "markov/performance/cpu_cost_model.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <utility>

namespace markov {

nlohmann::json WorkerCostFitter::fit(std::span<const WorkerCostSample> samples) const {
    using Key = std::pair<std::string, CpuSubmissionPart>;
    std::map<Key, CostDistribution> groups;
    for (const auto & sample : samples) groups[{ sample.operation, sample.part }].observe(sample.observed_ns);

    auto entries = nlohmann::json::array();
    for (const auto & [key, distribution] : groups) {
        auto entry = distribution.describe();
        entry["operation"] = key.first;
        entry["part"] = SourceCpuWork::part_name(key.second);
        entries.push_back(std::move(entry));
    }
    return entries;
}

nlohmann::json WorkerCostFitter::evaluate(const nlohmann::json & entries, std::span<const WorkerCostSample> samples) const {
    using Key = std::pair<std::string, CpuSubmissionPart>;
    const CpuCostModel estimates(CpuCostUnit::Worker, entries);

    CostErrors total;
    std::map<Key, CostErrors> groups;
    std::map<Key, std::size_t> unmatched;
    for (const auto & sample : samples) {
        const Key key{ sample.operation, sample.part };
        const auto estimate = estimates.worker_cost(sample.operation, sample.part);
        if (!estimate) {
            ++unmatched[key];
            continue;
        }
        total.observe(*estimate, sample.observed_ns);
        groups[key].observe(*estimate, sample.observed_ns);
    }

    auto reports = nlohmann::json::array();
    for (const auto & [key, errors] : groups)
        reports.push_back({
            { "operation",                            key.first },
            {      "part", SourceCpuWork::part_name(key.second) },
            {    "errors",                    errors.describe() }
        });
    auto missing = nlohmann::json::array();
    for (const auto & [key, count] : unmatched)
        missing.push_back({
            { "operation",                            key.first },
            {      "part", SourceCpuWork::part_name(key.second) },
            {     "count",                                count }
        });
    return {
        {   "overall",   total.describe() },
        {    "groups", std::move(reports) },
        { "unmatched", std::move(missing) }
    };
}

} // namespace markov
