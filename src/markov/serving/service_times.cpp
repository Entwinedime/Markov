#include "markov/serving/service_times.hpp"

#include <stdexcept>
#include <vector>

#include "markov/graph/graph.hpp"
#include "markov/templates/library.hpp"

namespace markov {

ServiceTimes::ServiceTimes(ServiceDuration given, ExecutionInputs * conditions, const nlohmann::json & assets)
    : given_(given), conditions_(conditions), assets_(assets) {}

void ServiceTimes::bind(const std::string & group, const TemplateLibrary & library) {
    if (!modeled()) return;

    const auto cost = library_costs_.try_emplace(&library, library, assets_).first;
    participants_.emplace(group, &cost->second);
}

std::optional<InvocationInput> ServiceTimes::action_input(const ServingAction & action) {
    if (!conditions_) return std::nullopt;
    return conditions_->invocation(action.task.group, "action", ExecutionInputs::action_identity(action));
}

GraphTiming ServiceTimes::action(const std::string & group, const CoarseGraph & graph, std::span<const TemplateInterval> origins,
                                 const std::optional<InvocationInput> & observed) {
    if (observed) return observed->timing(graph);
    if (given_) return { std::vector<ServiceDuration>(graph.work.size(), given_), {} };

    const auto began = std::chrono::steady_clock::now();
    auto result = participants_.at(group)->timing(graph, origins);
    query_time_ += std::chrono::steady_clock::now() - began;
    queries_ += graph.work.size();
    return result;
}

ServiceDuration ServiceTimes::external(const std::string & group, const nlohmann::json & identity, WorkKind kind, const WorkFeatures & features) {
    if (given_) return given_;

    const auto began = std::chrono::steady_clock::now();
    const auto duration = participants_.at(group)->external(identity, kind, features);
    query_time_ += std::chrono::steady_clock::now() - began;
    ++queries_;
    return duration;
}

nlohmann::json ServiceTimes::finish() const {
    if (conditions_) return conditions_->finish();
    if (given_) return {{ "duration_basis", "uniform_given_work_ns" }, { "given_work_ns", *given_ }};
    return {{ "duration_basis", "independent_service_costs" }, { "cost_queries", queries_ }, { "cost_query_ns", query_time_.count() }};
}

} // namespace markov
