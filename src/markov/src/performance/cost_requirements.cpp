#include "markov/performance/cost_requirements.hpp"

#include "markov/performance/cost_query.hpp"

#include <utility>

namespace markov {

void CostRequirements::record(const CostQuery & query, const NodeCostResult & measured, const nlohmann::json & location, const nlohmann::json & work_origin) {
    auto descriptor = query.describe();
    auto [found, inserted] = requirements_.try_emplace(descriptor.dump());
    auto & entry = found->second;
    if (inserted) {
        entry.object = std::move(descriptor);
        entry.example = location;
        entry.work_origin = work_origin;
        if (measured) entry.coverage = "candidate_available";
        else {
            auto gap = measured.error();
            if (gap.reason.empty()) gap = { query.issue, query.contract_gap };
            entry.coverage = gap.issue == CostQueryIssue::None ? "needs_data" : "contract_incomplete";
            entry.reason = gap.reason;
        }
    }
    ++entry.actual_services;
}

nlohmann::json CostRequirements::describe(bool diagnostics) const {
    auto rows = nlohmann::json::array();
    std::map<std::string, std::size_t> coverage;
    for (const auto & requirement : requirements_) {
        const auto & entry = requirement.second;
        ++coverage[entry.coverage];
        // Planning consumes gaps. Covered point details belong to diagnostics;
        // the summary still accounts for every instantiated measurement object.
        if (!diagnostics && entry.coverage == "candidate_available") continue;

        rows.push_back({
            {          "object",                                                                  entry.object },
            {        "coverage",                                                                entry.coverage },
            {          "reason", entry.reason.empty() ? nlohmann::json(nullptr) : nlohmann::json(entry.reason) },
            { "actual_services",                                                         entry.actual_services },
            {         "example",                                                                 entry.example },
            {     "work_origin",                                                             entry.work_origin }
        });
    }
    return {
        {          "scope",                                                "instantiated_target_work"                           },
        {  "qualification",                                                                                    "not_established" },
        { "object_details",                                                               diagnostics ? "all" : "uncovered_only" },
        {        "objects",                                                                                      std::move(rows) },
        {       "coverage",                                                                                             coverage },
        {    "limitations",
         { "candidate availability is not validated cost accuracy or complete target-domain coverage",
         "CPU work features require a measured-cost contract; explicit given durations do not establish that qualification" }   }
    };
}

} // namespace markov
