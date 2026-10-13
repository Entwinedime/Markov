#include "markov/performance/admission_cost_fitter.hpp"
#include "markov/performance/cost_statistics.hpp"
#include "markov/performance/cpu_cost_model.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <tuple>
#include <utility>

namespace markov {
nlohmann::json AdmissionCostFitter::fit(std::span<const AdmissionCostSample> samples) const {
    using Key = std::tuple<std::size_t, std::size_t, std::size_t>;
    std::map<Key, CostDistribution> groups;
    for (const auto & sample : samples) {
        const auto & work = sample.work;
        groups[{ work.accepted_requests, work.running_requests, work.horizon_checks }].observe(sample.observed_ns);
    }

    auto entries = nlohmann::json::array();
    for (auto & [key, values] : groups) {
        const auto [accepted, running, horizon] = key;

        // All observations remain in the distribution. Input length and rank
        // are not fitting keys; their transferability needs independent evidence.
        auto entry = nlohmann::json{
            {          "accepted_requests",      accepted },
            {           "running_requests",       running },
            {             "horizon_checks",       horizon },
            { "request_states_initialized", accepted != 0 },
        };
        entry.update(values.describe());
        entries.push_back(std::move(entry));
    }
    return entries;
}

nlohmann::json AdmissionCostFitter::evaluate(const nlohmann::json & entries, std::span<const AdmissionCostSample> samples) const {
    using Key = std::tuple<std::size_t, std::size_t, std::size_t>;
    const CpuCostModel estimates(CpuCostUnit::Admission, entries);

    CostErrors total;
    std::map<Key, CostErrors> groups;
    auto unmatched = nlohmann::json::array();
    for (const auto & sample : samples) {
        const auto & work = sample.work;
        const Key key{ work.accepted_requests, work.running_requests, work.horizon_checks };
        const auto found = estimates.admission_cost(work);
        if (!found) {
            unmatched.push_back(work.describe());
            continue;
        }
        total.observe(*found, sample.observed_ns);
        groups[key].observe(*found, sample.observed_ns);
    }

    auto reports = nlohmann::json::array();
    for (const auto & [key, errors] : groups) {
        const auto [accepted, running, horizon] = key;
        reports.push_back({
            { "accepted_requests",          accepted },
            {  "running_requests",           running },
            {    "horizon_checks",           horizon },
            {            "errors", errors.describe() }
        });
    }
    return {
        {   "overall", total.describe() },
        {    "groups",          reports },
        { "unmatched",        unmatched }
    };
}

} // namespace markov
