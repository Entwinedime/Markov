#include "markov/execution/simulation.hpp"

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
      execution_graph_(graph_path),
      graph_path_(std::move(graph_path)),
      request_timings_(requests.size()) {}

bool SimulationRuntime::LaterEvent::operator()(const QueuedEvent & a, const QueuedEvent & b) const {
    if (a.event.time_ns != b.event.time_ns) return a.event.time_ns > b.event.time_ns;
    return a.sequence > b.sequence;
}

void SimulationRuntime::enqueue(ExecutionEvent event) { events_.push({ std::move(event), next_event_sequence_++ }); }

std::optional<nlohmann::json> SimulationRuntime::accept(std::vector<ControlAction> actions) {
    std::vector<InvokeStep> invocations;
    for (auto & action : actions) {
        if (const auto * gap = std::get_if<UnsupportedControl>(&action))
            return report("unsupported",
                          Json::array({
                              { { "kind", "control" }, { "reason", gap->reason }, { "time_ns", time_ns_ } }
            }));

        invocations.push_back(std::move(std::get<InvokeStep>(action)));
    }
    if (invocations.empty()) {
        execution_graph_.retain(assembly_.frontier(), controller_.retained_ports());
        return std::nullopt;
    }

    auto batch = templates_.instantiate(invocations, request_capacity_);
    if (!batch) {
        const auto & gap = batch.error();
        const auto invocation = std::find_if(invocations.begin(), invocations.end(), [&gap](const auto & item) { return item.step == gap.step; });
        const bool missing_observation = gap.kind == TemplateGapKind::Observation;
        auto description = describe_invocation(*invocation);
        description["kind"] = missing_observation ? "observation" : "implementation";
        description["reason"] = gap.reason;
        description["rank"] = gap.rank;
        description["time_ns"] = time_ns_;
        return report(missing_observation ? "needs_data" : "unsupported", Json::array({ std::move(description) }));
    }

    std::vector<TemplateInstance *> instances;
    auto workspace = workspace_;
    for (std::size_t index = 0; index < invocations.size(); ++index) {
        for (auto & instance : (*batch)[index]) {
            instances.push_back(&instance);
            const auto advanced = workspace.advance(instance, templates_, *invocations[index].input, graph_path_.has_value());
            if (!advanced)
                return report("unsupported",
                              Json::array({
                                  { { "kind", "structure" }, { "reason", advanced.error() }, { "time_ns", time_ns_ } }
                }));
        }
    }
    auto assembly = assembly_;
    const auto connected = assembly.connect(instances);
    if (!connected)
        return report("unsupported",
                      Json::array({
                          { { "kind", "structure" }, { "reason", connected.error() }, { "time_ns", time_ns_ } }
        }));
    if (!execution_graph_.append(instances, invocations, time_ns_)) {
        record_batch(invocations);
        auto result = report("unsupported",
                             Json::array({
                                 { { "kind", "structure" }, { "reason", "target_dependency_cycle" }, { "time_ns", time_ns_ } }
        }));
        execution_graph_.release_batch();
        return result;
    }

    // Costs and completion times belong to this batch only. No failed batch
    // publishes controller events, resource frontiers or workspace effects.
    auto completions = execution_graph_.schedule(costs_, invocations);
    record_batch(invocations);
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
        if (!gap.cost_query.is_null()) description["cost_query"] = gap.cost_query;
        auto result = report("needs_data", Json::array({ std::move(description) }));
        execution_graph_.release_batch();
        return result;
    }

    workspace_ = std::move(workspace);
    assembly_ = std::move(assembly);
    execution_graph_.retain(assembly_.frontier(), controller_.retained_ports());
    execution_graph_.release_batch();

    // Instances are local to acceptance. Output events own their small effects;
    // only control-port bookkeeping remains attached to dynamic work.
    for (const auto & invocation : invocations) {
        if (!work_.emplace(invocation.work_id, Work{}).second) throw std::logic_error("duplicate dynamic work identity");
    }
    work_count_ += invocations.size();
    peak_work_ = std::max(peak_work_, work_.size());
    if (invocations.front().input->phase != BatchPhase::Idle) ++batches_;
    for (auto & event : *completions) {
        if (const auto * completion = std::get_if<WorkPortCompleted>(&event.payload)) ++work_.at(completion->port.work_id).pending_ports;
        enqueue(std::move(event));
    }
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
            const auto & event = events_.top().event;
            if (const auto * output = std::get_if<RequestOutput>(&event.payload)) {
                auto & timing = request_timings_[output->request_id];
                if (!timing.first_published_ns) timing.first_published_ns = event.time_ns;
                if (output->finishes) timing.completed_ns = event.time_ns;
            }
            else if (const auto * ready = std::get_if<RequestReady>(&event.payload)) request_timings_[ready->request_id].ready_ns = event.time_ns;
            else if (const auto * arrival = std::get_if<RequestArrived>(&event.payload)) simultaneous.push_back({ event.time_ns, *arrival });
            else simultaneous.push_back({ event.time_ns, std::get<WorkPortCompleted>(event.payload) });
            events_.pop();
        }

        // Apply all physical completions before a controller sees this instant.
        // Arrivals at the same instant participate in that single decision.
        for (const auto & event : simultaneous) {
            const auto * completion = std::get_if<WorkPortCompleted>(&event.payload);
            if (!completion) continue;
            auto & work = work_.at(completion->port.work_id);
            --work.pending_ports;
        }

        if (!simultaneous.empty())
            if (auto stopped = accept(controller_.handle(simultaneous))) return std::move(*stopped);

        const auto retained = controller_.retained_ports();
        std::erase_if(work_, [&](const auto & entry) {
            return entry.second.pending_ports == 0 && std::ranges::none_of(retained, [&](const auto & port) { return port.work_id == entry.first; });
        });
    }

    if (controller_.finish().completed_requests != requests_.size()) {
        return report("unsupported",
                      Json::array({
                          { { "kind", "control" }, { "reason", "remaining_requests_cannot_be_admitted" }, { "time_ns", time_ns_ } }
        }));
    }

    return report("complete", Json::array());
}

nlohmann::json SimulationRuntime::describe_invocation(const InvokeStep & invocation) const {
    auto requests = Json::array();
    for (const auto & request : invocation.input->requests) {
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
    for (const auto & filter : invocation.input->filters) filters.push_back(filter.describe());
    const auto & input = *invocation.input;
    const ServingContract serving;
    Json result = {
        { "work_id", invocation.work_id },
        { "step", serving.block(invocation.step).name },
        { "phase", serving.phase_name(input.phase) },
        { "requests", std::move(requests) },
        { "free_kv_pages", input.free_kv_pages },
        { "batch_filters", std::move(filters) },
        { "batch_transition", input.transition.describe() },
        { "admission_entry", input.admission.describe() },
        { "admission_work", input.admission_work.describe(input.admission, input.phase == BatchPhase::Prefill ? input.requests.size() : 0) },
        { "batch_merge", input.merge ? input.merge->describe() : Json(nullptr) }
    };
    if (invocation.step == StepKind::Schedule) {
        auto incoming = Json::array();
        for (const auto & request : input.incoming_requests)
            incoming.push_back({
                {            "id", requests_[request.request_id].id },
                {    "request_id",               request.request_id },
                { "prompt_tokens",            request.prompt_tokens },
                { "output_tokens",            request.output_tokens }
            });
        result["incoming_requests"] = std::move(incoming);
    }
    return result;
}

void SimulationRuntime::record_batch(std::span<const InvokeStep> invocations) {
    Json description = nullptr;
    if (graph_path_) {
        description = Json::array();
        for (const auto & invocation : invocations) description.push_back(describe_invocation(invocation));
    }
    execution_graph_.record_batch(description);
}

std::optional<nlohmann::json> SimulationRuntime::request_result(RequestId id) const {
    const auto & request = requests_[id];
    const auto & timing = request_timings_[id];
    if (!timing.completed_ns) return std::nullopt;
    const auto ready = *timing.ready_ns;
    const auto first = *timing.first_published_ns;
    const auto last = *timing.completed_ns;
    return Json{
        {                        "id",                                                                                                         request.id },
        {      "scheduled_arrival_ns",                                                                                            request.arrival_time_ns },
        {        "scheduler_ready_ns",                                                                                                              ready },
        {        "first_published_ns",                                                                                                              first },
        {              "completed_ns",                                                                                                               last },
        {                   "ttft_ns",                                                                                                      first - ready },
        {        "request_latency_ns",                                                                                                       last - ready },
        {            "driver_ttft_ns",                                                                                    first - request.arrival_time_ns },
        { "driver_request_latency_ns",                                                                                     last - request.arrival_time_ns },
        {                   "tpot_ns", request.output_tokens == 1 ? Json(nullptr) : Json(static_cast<double>(last - first) / (request.output_tokens - 1)) }
    };
}

nlohmann::json SimulationRuntime::report(std::string_view status, const nlohmann::json & gaps) {
    execution_graph_.finish();
    Json metrics = nullptr;
    if (status == "complete") {
        const auto start_ns = std::min_element(requests_.begin(), requests_.end(), [](const Request & a, const Request & b) {
                                  return a.arrival_time_ns < b.arrival_time_ns;
                              })->arrival_time_ns;
        std::uint64_t tokens = 0;
        auto last_published_ns = start_ns;
        for (RequestId id = 0; id < requests_.size(); ++id) {
            const auto & request = requests_[id];
            const auto & timing = request_timings_[id];
            const auto last = *timing.completed_ns;
            tokens += request.output_tokens;
            last_published_ns = std::max(last_published_ns, last);
        }

        // Private asynchronous tails can outlive the final visible response.
        const auto window_ns = last_published_ns - start_ns;
        metrics = {
            {        "window_time_ns",                                                                            window_ns },
            { "output_tokens_per_sec", window_ns == 0 ? Json(nullptr) : Json(static_cast<double>(tokens) * 1e9 / window_ns) },
            {         "request_count",                                                                     requests_.size() }
        };
    }

    Json result = {
        {            "status",                                                          status                              },
        {             "scope",                                                                                     "modeled" },
        {           "metrics",                                                                                       metrics },
        {              "gaps",                                                                                          gaps },
        {      "timing_basis", costs_.has_models() ? "empirical_candidates_with_conditional_inputs" : "given_node_durations" },
        {       "assumptions",                                                                                 Json::array() },
        { "execution_summary",
         { { "work_nodes", work_count_ },
         { "batch_invocations", batches_ },
         { "simulated_until_ns", time_ns_ },
         { "active_work", work_.size() },
         { "peak_active_work", peak_work_ } }                                                                               }
    };

    result["structure"] = {
        { "status", status }
    };
    result["structure"]["graph"] = execution_graph_.describe();
    result["performance_requirements"] = requirements().summary();
    result["performance_requirements"]["expansion_complete"] = status == "complete";
    result["performance_requirements"]["timing_basis"] = result["timing_basis"];
    auto workspace = Json::array();
    for (const auto & [resource, capacity] : workspace_.capacities())
        workspace.push_back({
            {       "resource", resource.describe() },
            { "capacity_bytes",            capacity }
        });
    result["structure"]["workspace_capacities"] = std::move(workspace);

    if (graph_path_) { result["structure"]["graph"]["artifact"] = graph_path_->string(); }

    return result;
}

} // namespace markov
