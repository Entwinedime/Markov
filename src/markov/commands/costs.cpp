#include "markov/commands/costs.hpp"

#include <map>
#include <stdexcept>
#include <string>

namespace markov::commands {

nlohmann::json cost_report(const adapters::CostQueries & queries, const adapters::SelectedCosts & selected) {
    using namespace trace_graph::cost;
    using Json = nlohmann::json;
    if (queries.deployment != selected.deployment) throw std::runtime_error("Selected cost models do not match the requested deployment");

    static const std::map<Status, std::string> statuses{
        {                 Status::Ready,                  "ready" },
        {          Status::MissingModel,          "missing_model" },
        {       Status::MissingFeatures,       "missing_features" },
        {         Status::OutsideDomain,         "outside_domain" },
        { Status::UnmeasuredCombination, "unmeasured_combination" },
        {       Status::InvalidResponse,       "invalid_response" }
    };
    static const std::map<Coverage, std::string> coverage{
        {       Coverage::Measured,     "measured_work_point" },
        { Coverage::NewCombination, "new_feature_combination" },
        {   Coverage::Extrapolated,            "extrapolated" }
    };
    auto results = Json::array();
    auto used_models = Json::object();
    std::map<std::string, std::size_t> counts;
    for (std::size_t i = 0; i < queries.work.size(); ++i) {
        const auto & work = queries.work[i];
        const auto response = selected.models.query(work);
        ++counts[statuses.at(response.status)];
        results.push_back({
            {              "work",                                                                         i },
            {         "operation",                                                       work.kind.operation },
            {            "status",                                              statuses.at(response.status) },
            {       "duration_ns",        response.duration_ns ? Json(*response.duration_ns) : Json(nullptr) },
            {          "coverage", response.coverage ? Json(coverage.at(*response.coverage)) : Json(nullptr) },
            { "affected_features",                                                response.affected_features },
            {             "model",                    response.model ? Json(*response.model) : Json(nullptr) }
        });
        if (response.model) {
            const auto key = std::to_string(*response.model);
            if (!used_models.contains(key))
                used_models[key] = {
                    { "sources", selected.models.model(*response.model).sources }
                };
        }
    }
    return {
        {          "status",                                                                              "cost_queries_completed" },
        {          "models",                                                                          queries.models_path.string() },
        { "selected_models",                                                                                selected.models.size() },
        {      "deployment",                                                                                   selected.deployment },
        {         "summary",                                                                                                counts },
        {            "work",                                                                                    std::move(results) },
        {     "used_models",                                                                                std::move(used_models) },
        {   "e2e_predicted",                                                                                                 false },
        {   "qualification", "Measured work coordinates describe coverage, not held-out accuracy. No target trace or E2E is read." }
    };
}

} // namespace markov::commands
