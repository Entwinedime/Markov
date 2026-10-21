#include "markov/contracts/control.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace markov {

nlohmann::json BatchTransitionState::describe() const {
    return {
        {         "running_requests",                                                                               running_requests },
        { "pending_prefill_requests", pending_prefill_requests ? nlohmann::json(*pending_prefill_requests) : nlohmann::json(nullptr) },
        {        "admission_blocked",                                                                              admission_blocked }
    };
}

nlohmann::json AdmissionEntryState::describe() const {
    return {
        {  "waiting_requests",  waiting_requests },
        {  "running_requests",  running_requests },
        { "admission_blocked", admission_blocked }
    };
}

std::size_t AdmissionWork::horizon_checks() const { return std::accumulate(request_horizon_checks.begin(), request_horizon_checks.end(), std::size_t{ 0 }); }

std::string_view AdmissionWork::stop_name(AdmissionStop stop) {
    static constexpr std::array names{ "queue_exhausted", "closed", "no_waiting_requests", "request_capacity", "memory_budget", "input_budget" };
    return names[static_cast<std::size_t>(stop)];
}

bool AdmissionWork::parameterized_cpu_path(const AdmissionEntryState & entry, std::size_t admitted_requests) const {
    if (admitted_requests == 0) {
        if (candidate_checks != 0 || !request_horizon_checks.empty()) return false;
        if (entry.admission_blocked) return stop == AdmissionStop::Closed;
        if (entry.waiting_requests == 0) return stop == AdmissionStop::NoWaitingRequests;
        return stop == AdmissionStop::RequestCapacity;
    }

    if (entry.admission_blocked || request_horizon_checks.size() != admitted_requests
        || std::ranges::find(request_horizon_checks, 0) != request_horizon_checks.end())
        return false;
    if (stop == AdmissionStop::QueueExhausted) return entry.waiting_requests == admitted_requests && candidate_checks == admitted_requests;
    // Input-budget exhaustion follows a committed request. It has no rejected
    // candidate or persistent admission latch, unlike a memory-budget failure.
    if (stop == AdmissionStop::InputBudget) return entry.waiting_requests >= admitted_requests && candidate_checks == admitted_requests;
    return stop == AdmissionStop::RequestCapacity && entry.waiting_requests > admitted_requests && candidate_checks == admitted_requests + 1;
}

nlohmann::json AdmissionWork::describe(const AdmissionEntryState & entry, std::size_t admitted_requests) const {
    // Native compaction and the post-Prepare pending-token sum only run when a
    // new prefill batch exists. Derive their sizes instead of storing queue state.
    return {
        {         "candidate_checks",                                                        candidate_checks },
        {           "request_checks",                                           request_horizon_checks.size() },
        {           "horizon_checks",                                                        horizon_checks() },
        {   "request_horizon_checks",                                                  request_horizon_checks },
        {                     "stop",                                                         stop_name(stop) },
        {   "queue_compaction_items",                     admitted_requests == 0 ? 0 : entry.waiting_requests },
        { "pending_statistics_items", admitted_requests == 0 ? 0 : entry.waiting_requests - admitted_requests }
    };
}

nlohmann::json BatchFilter::describe() const {
    return {
        {      "input_count",      input_count },
        { "retained_indices", retained_indices }
    };
}

nlohmann::json BatchMerge::describe() const {
    return {
        {  "left_count",  left_count },
        { "right_count", right_count }
    };
}

std::string_view ServingContract::phase_name(BatchPhase phase) const {
    switch (phase) {
    case BatchPhase::Prefill:
        return "prefill";
    case BatchPhase::Decode:
        return "decode";
    case BatchPhase::Idle:
        return "idle";
    }
    std::unreachable();
}

std::span<const BlockDefinition> ServingContract::steps() const {
    static constexpr std::array schedule_ports{ PortKind::Selected };
    static constexpr std::array prepare_ports{ PortKind::Prepared };
    static constexpr std::array execution_ports{ PortKind::Submitted, PortKind::DeviceDone };
    static constexpr std::array publish_ports{ PortKind::ResultProcessed };

    static constexpr std::array schedule_inputs{
        BlockParameter{ "visible_requests",      BlockParameterKind::RequestIds },
        BlockParameter{ "admission_policy", BlockParameterKind::AdmissionPolicy },
        BlockParameter{  "worker_controls",  BlockParameterKind::WorkerControls }
    };
    static constexpr std::array batch_inputs{
        BlockParameter{ "batch_id", BlockParameterKind::BatchIdentity },
        BlockParameter{    "phase",         BlockParameterKind::Phase },
        BlockParameter{ "requests",  BlockParameterKind::RequestBatch }
    };

    static constexpr BlockDefinition schedule{ StepKind::Schedule, "schedule",         "arrival or previous result_processed",
                                               schedule_ports,     PortKind::Selected, "commit batch selection or an empty scheduling decision",
                                               schedule_inputs };
    static constexpr BlockDefinition prepare{ StepKind::Prepare, "prepare", "selected", prepare_ports, PortKind::Prepared, "input and KV metadata ready",
                                              batch_inputs };
    static constexpr BlockDefinition prefill{ StepKind::Prefill, "prefill",           "prepared",
                                              execution_ports,   PortKind::Submitted, "device_done makes prefill result ready",
                                              batch_inputs };
    static constexpr BlockDefinition decode{
        StepKind::Decode, "decode", "prepared and previous result_processed", execution_ports, PortKind::Submitted, "device_done makes next result ready",
        batch_inputs
    };
    static constexpr BlockDefinition publish{ StepKind::Publish,
                                              "publish",
                                              "submitted; device_done gates result_processed",
                                              publish_ports,
                                              PortKind::ResultProcessed,
                                              "commit generated tokens and finished request KV; emit request_output only on token handoff",
                                              batch_inputs };

    static constexpr std::array contracts{ schedule, prepare, prefill, decode, publish };

    return contracts;
}

const BlockDefinition & ServingContract::block(StepKind kind) const {
    for (const auto & definition : steps()) {
        if (definition.kind == kind) return definition;
    }

    std::unreachable();
}

const BlockDefinition & ServingContract::block(std::string_view name) const {
    for (const auto & definition : steps()) {
        if (definition.name == name) return definition;
    }

    throw std::invalid_argument("unknown serving block: " + std::string(name));
}

nlohmann::json ServingContract::describe() const {
    auto definitions = nlohmann::json::array();
    for (const auto & definition : steps()) {
        auto ports = nlohmann::json::array();
        for (const auto port : definition.exits) ports.push_back(port_name(port));

        auto parameters = nlohmann::json::array();
        for (const auto & parameter : definition.parameters) {
            std::string_view type;
            switch (parameter.kind) {
            case BlockParameterKind::RequestIds:
                type = "request_ids";
                break;
            case BlockParameterKind::BatchIdentity:
                type = "batch_identity";
                break;
            case BlockParameterKind::Phase:
                type = "phase";
                break;
            case BlockParameterKind::RequestBatch:
                type = "request_batch";
                break;
            case BlockParameterKind::AdmissionPolicy:
                type = "admission_policy";
                break;
            case BlockParameterKind::WorkerControls:
                type = "worker_controls";
                break;
            }
            parameters.push_back({
                { "name", parameter.name },
                { "type",           type }
            });
        }

        definitions.push_back({
            {     "parameters",                           parameters },
            {           "step",                      definition.name },
            {          "entry",                     definition.entry },
            {     "exit_ports",                                ports },
            { "cpu_completion", port_name(definition.cpu_completion) },
            {         "effect",                    definition.effect }
        });
        if (definition.kind == StepKind::Publish)
            definitions.back()["request_output"] = {
                { "boundary", "sending role batch_output return after token handoff to detokenizer" },
                {  "members",               "only requests with a positive emitted token increment" }
            };
    }

    return definitions;
}

std::string_view ServingContract::port_name(PortKind port) const {
    switch (port) {
    case PortKind::Selected:
        return "selected";
    case PortKind::Prepared:
        return "prepared";
    case PortKind::Submitted:
        return "submitted";
    case PortKind::DeviceDone:
        return "device_done";
    case PortKind::ResultProcessed:
        return "result_processed";
    }

    std::unreachable();
}

} // namespace markov
