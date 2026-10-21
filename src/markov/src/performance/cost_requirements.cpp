#include "markov/performance/cost_requirements.hpp"

#include <utility>

namespace markov {
namespace {

using Json = nlohmann::json;

Json work_origin(const InvokeStep & invocation) {
    const ServingContract contract;
    auto members = Json::array();
    for (const auto & request : invocation.input->requests) {
        members.push_back({
            {       "request_id",       request.request_id },
            {     "input_tokens",     request.input_tokens },
            {   "context_tokens",   request.context_tokens },
            { "generated_tokens", request.generated_tokens },
            {         "finishes",         request.finishes }
        });
    }

    auto filters = Json::array();
    for (const auto & filter : invocation.input->filters) filters.push_back(filter.describe());
    Json origin{
        {          "step",         contract.block(invocation.step).name },
        {         "phase", contract.phase_name(invocation.input->phase) },
        {      "requests",                           std::move(members) },
        { "batch_filters",                           std::move(filters) }
    };
    if (invocation.input->merge) origin["batch_merge"] = invocation.input->merge->describe();
    return origin;
}

} // namespace

void CostRequirements::record(const CostQuery & query, const NodeCostResult & measured, const InvokeStep & invocation, std::shared_ptr<const Json> & origin) {
    auto [found, inserted] = requirements_.try_emplace(query);
    auto & entry = found->second;
    ++entry.actual_services;
    if (!inserted) return;

    if (measured) entry.coverage = "candidate_available";
    else {
        auto gap = measured.error();
        if (gap.reason.empty()) gap = { query.issue, query.contract_gap };
        entry.coverage = gap.issue == CostQueryIssue::None ? "needs_data" : "contract_incomplete";
        entry.reason = gap.reason;
    }

    // A batch's native recipe is shared by its distinct missing cost objects.
    // Covered work needs no sampling recipe or duplicate diagnostic snapshot.
    if (!measured) {
        if (!origin) origin = std::make_shared<const Json>(work_origin(invocation));
        entry.origin = origin;
    }
}

Json CostRequirements::summary() const {
    std::map<std::string, std::size_t> coverage;
    std::map<std::string, std::size_t> kinds;
    auto examples = Json::array();
    std::size_t services = 0;
    std::size_t uncovered = 0;
    for (const auto & [query, entry] : requirements_) {
        ++coverage[entry.coverage];
        ++kinds[std::string(CostQuery::kind_name(query.kind))];
        services += entry.actual_services;
        if (entry.coverage == "candidate_available") continue;
        ++uncovered;
        if (examples.size() == 5) continue;
        examples.push_back({
            {     "kind", CostQuery::kind_name(query.kind) },
            { "coverage",                   entry.coverage },
            {   "reason",                     entry.reason }
        });
    }
    return {
        {           "scope",                                                                 "instantiated_target_work" },
        {   "qualification",                                                                          "not_established" },
        {    "object_count",                                                                       requirements_.size() },
        { "actual_services",                                                                                   services },
        {        "coverage",                                                                                   coverage },
        {           "kinds",                                                                                      kinds },
        {        "examples", { { "shown", examples.size() }, { "total", uncovered }, { "items", std::move(examples) } } }
    };
}

Json CostRequirements::data() const {
    auto objects = Json::array();
    auto origins = Json::array();
    std::map<const Json *, std::size_t> indices;
    for (const auto & [query, entry] : requirements_) {
        if (entry.coverage == "candidate_available") continue;
        const auto [origin, inserted] = indices.try_emplace(entry.origin.get(), origins.size());
        if (inserted) origins.push_back(*entry.origin);
        objects.push_back({
            {          "object",                                          query.describe() },
            {        "coverage",                                            entry.coverage },
            {          "reason", entry.reason.empty() ? Json(nullptr) : Json(entry.reason) },
            { "actual_services",                                     entry.actual_services },
            {     "work_origin",                                            origin->second }
        });
    }
    return {
        { "objects", std::move(objects) },
        { "origins", std::move(origins) }
    };
}

} // namespace markov
