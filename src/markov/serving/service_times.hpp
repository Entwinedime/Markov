#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>

#include <nlohmann/json.hpp>

#include "markov/execution/timing.hpp"
#include "markov/performance/work_costs.hpp"
#include "markov/serving/execution_inputs.hpp"

namespace markov {

class ServiceTimes {
public:
    ServiceTimes(ServiceDuration given, ExecutionInputs * conditions, const nlohmann::json & assets);

    void bind(const std::string & group, nlohmann::json domain, std::span<const TemplateDefinition> definitions);
    bool modeled() const { return !assets_.is_null(); }
    ExecutionInputs * conditions() const { return conditions_; }
    std::optional<InvocationInput> action_input(const ServingAction & action);
    GraphTiming action(const std::string & group, const CoarseGraph & graph, std::span<const TemplateInterval> origins,
                       const std::optional<InvocationInput> & observed);
    ServiceDuration external(const std::string & group, const nlohmann::json & identity, WorkKind kind, const WorkFeatures & features);
    nlohmann::json finish() const;

private:
    ServiceDuration given_;
    ExecutionInputs * conditions_;
    const nlohmann::json & assets_;
    std::map<std::string, WorkCosts> participants_;
    std::size_t queries_ = 0;
    std::chrono::nanoseconds query_time_{};
};

} // namespace markov
