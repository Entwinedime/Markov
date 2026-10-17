#include "markov/execution/simulation.hpp"

#include "markov/execution/execution_details.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace markov {
namespace {

using Json = nlohmann::json;

} // namespace

SimulationRuntime::SimulationRuntime(std::span<const Request> requests, Controller & controller, const TemplateBank & templates, const NodeCosts & costs,
                                     std::optional<std::filesystem::path> graph_path)
    : requests_(requests),
      controller_(controller),
      templates_(templates),
      costs_(costs),
      workspace_(templates.workspace_pools()),
      graph_path_(std::move(graph_path)),
      request_timings_(requests.size()) {}

bool SimulationRuntime::LaterEvent::operator()(const QueuedEvent & a, const QueuedEvent & b) const {
    if (a.event.time_ns != b.event.time_ns) return a.event.time_ns > b.event.time_ns;
    return a.sequence > b.sequence;
}

void SimulationRuntime::enqueue(ControlEvent event) { events_.push({ std::move(event), next_event_sequence_++ }); }

std::optional<nlohmann::json> SimulationRuntime::accept(std::span<const ControlAction> actions) {
    std::vector<InvokeStep> invocations;
    for (const auto & action : actions) {
        if (const auto * gap = std::get_if<UnsupportedControl>(&action))
            return report("unsupported",
                          Json::array({
                              { { "kind", "control" }, { "reason", gap->reason }, { "time_ns", time_ns_ } }
            }));

        if (const auto * completion = std::get_if<CompleteRequest>(&action)) {
            request_timings_[completion->request_id].completed_ns = work_.at(completion->published.work_id).completed_ports.at(PortKind::Published);
            continue;
        }

        invocations.push_back(std::get<InvokeStep>(action));
    }
    if (invocations.empty()) return std::nullopt;

    auto batch = templates_.instantiate(invocations, request_capacity_);
    if (!batch) {
        const auto & gap = batch.error();
        const auto invocation = std::find_if(invocations.begin(), invocations.end(), [&gap](const auto & item) { return item.step == gap.step; });
        const bool missing_observation = gap.kind == TemplateGapKind::Observation;
        auto requests = Json::array();
        for (const auto & request : invocation->input.requests) {
            requests.push_back({
                {               "id", requests_[request.request_id].id },
                {     "input_tokens",             request.input_tokens },
                {   "context_tokens",           request.context_tokens },
                { "generated_tokens",         request.generated_tokens },
                {         "finishes",                 request.finishes },
                {        "pool_slot",                request.pool_slot },
                {         "kv_pages",                 request.kv_pages }
            });
        }

        auto filters = Json::array();
        for (const auto & filter : invocation->input.filters) filters.push_back(filter.describe());

        return report(
            missing_observation ? "needs_data" : "unsupported",
            Json::array({
                { { "kind", missing_observation ? "observation" : "implementation" },
                 { "reason", gap.reason },
                 { "step", ServingContract{}.block(gap.step).name },
                 { "phase", ServingContract{}.phase_name(gap.phase) },
                 { "rank", gap.rank },
                 { "requests", requests },
                 { "batch_filters", filters },
                 { "batch_transition", invocation->input.transition.describe() },
                 { "admission_entry", invocation->input.admission.describe() },
                 { "admission_work",
                    invocation->input.admission_work.describe(invocation->input.admission,
                                                              invocation->input.phase == BatchPhase::Prefill ? invocation->input.requests.size() : 0) },
                 { "batch_merge", invocation->input.merge ? invocation->input.merge->describe() : Json(nullptr) },
                 { "time_ns", time_ns_ } }
        }));
    }

    std::vector<Work> staged;
    for (std::size_t index = 0; index < invocations.size(); ++index) staged.push_back({ invocations[index], std::move((*batch)[index]), time_ns_, {} });

    std::vector<TemplateInstance *> instances;
    for (auto & work : staged)
        for (auto & instance : work.instances) instances.push_back(&instance);
    auto workspace = workspace_;
    for (auto & work : staged) {
        for (auto & instance : work.instances) {
            const auto advanced = workspace.advance(instance, templates_, work.invocation.input);
            if (!advanced)
                return report("unsupported",
                              Json::array({
                                  { { "kind", "structure" }, { "reason", advanced.error() }, { "time_ns", time_ns_ } }
                }));
        }
    }
    const auto connected = assembly_.connect(instances);
    if (!connected)
        return report("unsupported",
                      Json::array({
                          { { "kind", "structure" }, { "reason", connected.error() }, { "time_ns", time_ns_ } }
        }));
    if (!execution_graph_.append(instances, invocations, time_ns_)) {
        return report("unsupported",
                      Json::array({
                          { { "kind", "structure" }, { "reason", "target_dependency_cycle" }, { "time_ns", time_ns_ } }
        }));
    }

    // Commit once after the whole batch has a valid graph. The current
    // non-overlap controller cannot admit the next model batch before completion.
    workspace_ = std::move(workspace);
    for (auto & work : staged) {
        const auto id = work.invocation.work_id;
        if (!work_.emplace(id, std::move(work)).second) throw std::logic_error("duplicate dynamic work identity");
    }
    if (invocations.front().input.phase != BatchPhase::Idle) ++batches_;

    // Work owns the immutable invocation. Execution resolves target service
    // quantities from it after staged instances have reached their stable owner.
    std::map<WorkId, const InvokeStep *> cost_contexts;
    for (const auto & [id, work] : work_) cost_contexts.emplace(id, &work.invocation);
    auto completions = execution_graph_.schedule(costs_, cost_contexts);
    if (!completions) {
        const auto & gap = completions.error();
        Json description{
            {            "kind",                                 gap.source_span ? "input" : "observation" },
            {          "reason",                                                                gap.reason },
            {         "work_id",                                                                  gap.work },
            {            "rank",                                                                  gap.rank },
            {     "source_span",                  gap.source_span ? Json(*gap.source_span) : Json(nullptr) },
            { "source_manifest", gap.source_manifest ? Json(gap.source_manifest->string()) : Json(nullptr) },
            {         "time_ns",                                                                  time_ns_ }
        };
        if (!gap.service_work.is_null()) description["service_work"] = gap.service_work;
        return report("needs_data", Json::array({ std::move(description) }));
    }
    for (auto & event : *completions) enqueue(std::move(event));
    return std::nullopt;
}

nlohmann::json SimulationRuntime::run(ServingLimits limits) {
    request_capacity_ = limits.request_capacity;
    controller_.initialize(requests_, limits);
    for (RequestId id = 0; id < requests_.size(); ++id) enqueue({ requests_[id].arrival_time_ns, RequestArrived{ id } });

    while (!events_.empty()) {
        time_ns_ = events_.top().event.time_ns;
        std::vector<ControlEvent> simultaneous;
        while (!events_.empty() && events_.top().event.time_ns == time_ns_) {
            simultaneous.push_back(events_.top().event);
            events_.pop();
        }

        // Apply all physical completions before a controller sees this instant.
        // Arrivals at the same instant participate in that single decision.
        for (const auto & event : simultaneous) {
            const auto * completion = std::get_if<WorkPortCompleted>(&event.payload);
            if (!completion) continue;
            auto & work = work_.at(completion->port.work_id);
            work.completed_ports.emplace(completion->port.port, time_ns_);
            if (completion->port.port == PortKind::Published) {
                for (const auto & request : work.invocation.input.requests) {
                    auto & timing = request_timings_[request.request_id];
                    if (!timing.first_published_ns) timing.first_published_ns = time_ns_;
                }
            }
        }

        std::vector<WorkId> in_flight;
        for (const auto & [id, work] : work_) {
            if (work.completed_ports.size() < work.instances.front().definition->ports.size()) in_flight.push_back(id);
        }

        const auto actions = controller_.handle(simultaneous, RuntimeView{ time_ns_, in_flight });
        if (auto stopped = accept(actions)) return std::move(*stopped);
    }

    if (controller_.finish().completed_requests != requests_.size()) {
        return report("unsupported",
                      Json::array({
                          { { "kind", "control" }, { "reason", "remaining_requests_cannot_be_admitted" }, { "time_ns", time_ns_ } }
        }));
    }

    return report("complete", Json::array());
}

nlohmann::json SimulationRuntime::report(std::string_view status, const nlohmann::json & gaps) const {
    auto connection_rules = Json::object();
    std::optional<ExecutionDetails> details;
    if (graph_path_) details.emplace(requests_);
    for (const auto & [id, work] : work_) {
        for (const auto & instance : work.instances) {
            for (const auto & target : instance.connections) {
                const std::string name(ExecutionDetails::connection_rule_name(target.binding.rule));
                connection_rules[name] = connection_rules.value(name, std::size_t{ 0 }) + 1;
            }
        }
        if (details) details->add(work.invocation, work.instances, work.completed_ports, work.start_ns);
    }

    Json metrics = nullptr;
    if (status == "complete") {
        const auto start_ns = std::min_element(requests_.begin(), requests_.end(), [](const Request & a, const Request & b) {
                                  return a.arrival_time_ns < b.arrival_time_ns;
                              })->arrival_time_ns;
        const auto window_ns = time_ns_ - start_ns;

        auto requests = Json::array();
        std::uint64_t tokens = 0;
        for (RequestId id = 0; id < requests_.size(); ++id) {
            const auto & request = requests_[id];
            const auto & timing = request_timings_[id];
            const auto first = *timing.first_published_ns;
            const auto last = *timing.completed_ns;
            tokens += request.output_tokens;
            requests.push_back({
                {                 "id",                                                                                                         request.id },
                { "first_published_ns",                                                                                                              first },
                {       "completed_ns",                                                                                                               last },
                {            "ttft_ns",                                                                                    first - request.arrival_time_ns },
                { "request_latency_ns",                                                                                     last - request.arrival_time_ns },
                {            "tpot_ns", request.output_tokens == 1 ? Json(nullptr) : Json(static_cast<double>(last - first) / (request.output_tokens - 1)) }
            });
        }

        metrics = {
            {        "window_time_ns",                                                                            window_ns },
            { "output_tokens_per_sec", window_ns == 0 ? Json(nullptr) : Json(static_cast<double>(tokens) * 1e9 / window_ns) },
            {              "requests",                                                                             requests }
        };
    }

    Json result = {
        {            "status",                                                                      status                              },
        {             "scope",                                                                                                 "modeled" },
        {           "metrics",                                                                                                   metrics },
        {              "gaps",                                                                                                      gaps },
        {      "timing_basis",             costs_.has_models() ? "empirical_candidates_with_conditional_inputs" : "given_node_durations" },
        {       "assumptions",
         { "explicit givens and/or empirical candidates; conditional execution, not a qualified performance prediction",
         "host control, preparation, submission and completion CPU service time is excluded; control state and necessary causality are retained",
         "device compute, communication, transfers including synchronous host copies, and device commands require costs",
         "waits follow dependencies; annotations carry no service charge",
         "initial modeled resources contain no in-flight work",
         "HTTP, tokenizer, initialization, warmup and idle polling are outside scope" }                                                 },
        { "execution_summary", { { "work_nodes", work_.size() }, { "batch_invocations", batches_ }, { "simulated_until_ns", time_ns_ } } }
    };

    result["structure"] = {
        {           "status",                                    status },
        {    "qualification", "pending_source_and_transform_validation" },
        { "connection_rules",               std::move(connection_rules) }
    };

    result["structure"]["graph"] = execution_graph_.describe();
    result["performance_requirements"] = execution_graph_.performance_requirements();
    result["performance_requirements"]["expansion_complete"] = status == "complete";
    result["performance_requirements"]["timing_basis"] = result["timing_basis"];
    auto workspace = Json::array();
    for (const auto & [resource, capacity] : workspace_.capacities())
        workspace.push_back({
            {       "resource", resource.describe() },
            { "capacity_bytes",            capacity }
        });
    result["structure"]["workspace_capacities"] = std::move(workspace);

    if (graph_path_) {
        std::filesystem::create_directories(graph_path_->parent_path());
        execution_graph_.write(*graph_path_);
        result["structure"]["graph"]["artifact"] = graph_path_->string();
        // Keep large per-work diagnostics out of the orchestration result.
        // Publish the sidecar atomically before exposing its reference.
        auto details_path = *graph_path_;
        details_path.replace_extension(".details.json");
        details->write(details_path);
        result["execution_details"] = {
            {   "artifact", details_path.string() },
            { "work_count",          work_.size() }
        };
    }

    return result;
}

} // namespace markov
