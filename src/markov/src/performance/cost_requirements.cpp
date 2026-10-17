#include "markov/performance/cost_requirements.hpp"

#include "markov/performance/cost_query.hpp"
#include "markov/performance/node_costs.hpp"

#include <utility>

namespace markov {

void CostRequirements::record(const CostQuery & query, const NodeCosts & costs, const nlohmann::json & location, const nlohmann::json & work_origin) {
    Requirement * measurable = nullptr;
    for (const auto & object : query.objects) {
        auto descriptor = object.describe();
        const auto key = descriptor.dump();
        auto [found, inserted] = requirements_.try_emplace(key);
        auto & entry = found->second;
        if (inserted) {
            entry.object = std::move(descriptor);
            entry.example = location;
            entry.work_origin = work_origin;
            // The same typed query drives both coverage and execution. A
            // producer-qualified traversal can resolve otherwise opaque work.
            const auto estimate = costs.query_cost(CostQuery{ { object } });
            if (estimate) entry.coverage = "candidate_available";
            else {
                auto gap = estimate.error();
                if (gap.reason.empty()) gap = { object.issue, object.contract_gap };
                entry.coverage = gap.issue == CostQueryIssue::None ? "needs_data" : "contract_incomplete";
                entry.reason = gap.reason;
            }
        }
        if (!measurable && entry.coverage == "needs_data") measurable = &entry;

        ++entry.actual_services;
    }

    // A physical service has one charge even if a query exposes more than one
    // compatible descriptor. Missing objects do not request duplicate costs.
    if (costs.query_cost(query) || !measurable) return;

    if (measurable->measurement_required_services == 0) {
        measurable->measurement_example = location;
        measurable->measurement_origin = work_origin;
    }
    ++measurable->measurement_required_services;
}

nlohmann::json CostRequirements::describe() const {
    auto rows = nlohmann::json::array();
    std::map<std::string, std::size_t> coverage;
    for (const auto & requirement : requirements_) {
        const auto & entry = requirement.second;
        ++coverage[entry.coverage];
        rows.push_back({
            {                        "object",                                                                  entry.object },
            {                      "coverage",                                                                entry.coverage },
            {                        "reason", entry.reason.empty() ? nlohmann::json(nullptr) : nlohmann::json(entry.reason) },
            {               "actual_services",                                                         entry.actual_services },
            {                       "example",                                                                 entry.example },
            {                   "work_origin",                                                             entry.work_origin },
            {          "measurement_required",                                      entry.measurement_required_services != 0 },
            { "measurement_required_services",                                           entry.measurement_required_services },
            {           "measurement_example",                                                     entry.measurement_example },
            {            "measurement_origin",                                                      entry.measurement_origin }
        });
    }
    return {
        {         "scope",    "instantiated_target_work"                          },
        { "qualification",                                       "not_established" },
        {       "objects",                                         std::move(rows) },
        {      "coverage",                                                coverage },
        {   "limitations",
         { "candidate availability is not validated cost accuracy or complete target-domain coverage",
         "host control service time is outside the current performance scope" }   }
    };
}

} // namespace markov
