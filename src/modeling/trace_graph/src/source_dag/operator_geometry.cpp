#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include "markov/trace_graph/source_dag/call_regions.hpp"
#include "markov/trace_graph/source_dag/observations.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace markov::trace_graph::source_dag {
namespace {

// The current Torch-NPU exporter inserts CR/LF after argument separators.
// Accept explicit JSON dimension arrays; unsupported textual encodings stay
// raw evidence, never inferred dimensions. An empty argument is unknown,
// whereas an explicit [] represents a zero-dimensional input.
nlohmann::json input_dimensions(std::string_view raw) {
    auto dimensions = nlohmann::json::array();
    while (true) {
        const auto separator = raw.find(';');
        const auto part = raw.substr(0, separator);
        auto shape = nlohmann::json::parse(part, nullptr, false);
        if (!shape.is_array() || !std::ranges::all_of(shape, [](const auto & value) {
                return value.is_number_unsigned() || (value.is_number_integer() && value.template get<int64_t>() >= 0);
            }))
            return nullptr;
        dimensions.push_back(std::move(shape));
        if (separator == std::string_view::npos) return dimensions;
        raw.remove_prefix(separator + 1);
    }
}

} // namespace

nlohmann::json operator_geometry_observations(const core::DagGraph & graph, const modules::hicache::HiCachePhaseObservationAudit & phases) {
    const modules::hicache::patch::HiCacheSourceDagIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    auto calls = nlohmann::json::array();
    auto metadata_calls = nlohmann::json::array();
    const auto metadata_regions = observe_attention_metadata_regions(source, queues);
    const auto phase_positions = phase_call_positions(phases);
    std::map<size_t, const AttentionMetadataRegion *> metadata_by_observation;
    for (const auto & region : metadata_regions) metadata_by_observation.emplace(region.work.observation, &region);

    struct Envelope {
        size_t row;
        const core::TraceEvent * event;
        uint64_t begin_ns, end_ns;
    };
    std::map<size_t, std::vector<Envelope>> device_envelopes;
    size_t parsed = 0;
    for (size_t i = 0; i < graph.runtime_observations().size(); ++i) {
        const auto & event = graph.runtime_observations()[i];
        const bool metadata = event.source_channel == core::TraceSourceChannel::PythonProbe && event.name == "runtime.phase.attention_metadata";
        const bool shaped_operator = event.source_channel == core::TraceSourceChannel::Torch && event.cat == "cpu_op" && event.has_arg("Input Dims");
        if (!metadata && !shaped_operator) continue;
        const auto position = measurement_position(event);
        if (!position) continue;

        const auto begin_ns = core::checked_add_u64(core::checked_multiply_u64(event.ts, 1'000, "operator timestamp overflow"),
                                                    event.ts_submicro_ns,
                                                    "operator timestamp overflow");
        const auto duration_ns = core::checked_add_u64(core::checked_multiply_u64(event.dur, 1'000, "operator duration overflow"),
                                                       event.dur_submicro_ns,
                                                       "operator duration overflow");
        const auto end_ns = core::checked_add_u64(begin_ns, duration_ns, "operator end overflow");
        const auto region = metadata ? metadata_by_observation.at(i)->work : observe_submitted_call_region(source, queues, i);
        auto devices = nlohmann::json::array();
        if (!region.caller.has_node_overlap) {
            for (const auto node : region.submitted_nodes) {
                if (graph.node(node).is_cpu) continue;
                const auto & kernel = graph.event_for_node(node);
                const auto kernel_position = measurement_position(kernel);
                if (!kernel_position) continue;
                devices.push_back({
                    {    "source_node",             node },
                    {       "position", *kernel_position },
                    { "implementation",      kernel.name }
                });
                if (shaped_operator) { device_envelopes[node].push_back({ calls.size(), &event, begin_ns, end_ns }); }
            }
        }
        if (metadata) {
            nlohmann::json phase_position = nullptr;
            const auto metadata_inputs = observed_attention_metadata_inputs(event);
            const auto owner = call_phase_position(region, phase_positions);
            if (owner) {
                const auto & phase = phases.observations.at(owner->phase_index);
                phase_position = {
                    { "logical_input",                            phase.logical_input },
                    {   "request_ids",                              phase.request_ids },
                    {         "phase", owner->decode_iteration ? "decode" : "prefill" }
                };
                phase_position["context_tokens"] = nullptr;
                if (metadata_inputs) phase_position["context_tokens"] = metadata_inputs->context_tokens;
                if (owner->decode_iteration) phase_position["decode_iteration"] = *owner->decode_iteration;
            }

            nlohmann::json metadata_program = nullptr;
            if (owner && metadata_inputs) {
                const auto program = structure::hicache::ascend_attention_metadata_program(owner->decode_iteration ? "decode" : "prefill", *metadata_inputs);
                if (program) {
                    metadata_program = nlohmann::json::array();
                    const auto matched = match_tensor_operators(source, queues, metadata_by_observation.at(i)->operator_observations, *program);
                    for (size_t index = 0; index < program->size(); ++index) {
                        const auto & operation = program->at(index);
                        auto launches = nlohmann::json::array();
                        for (const auto & launch : operation.device_launches)
                            launches.push_back({
                                {      "operation",      launch.operation },
                                { "implementation", launch.implementation },
                                {       "features",       launch.features }
                            });
                        nlohmann::json measured_position = nullptr;
                        if (const auto found = matched.find(index); found != matched.end())
                            if (const auto value = measurement_position(graph.runtime_observations().at(found->second))) measured_position = *value;
                        metadata_program.push_back({
                            {         "operation",                              operation.work.operation },
                            {    "implementation",                         operation.work.implementation },
                            {          "features",                               operation.work.features },
                            {         "execution", structure::tensor_execution_name(operation.execution) },
                            {      "predecessors",                                operation.predecessors },
                            {   "device_launches",                                   std::move(launches) },
                            { "operator_position",                          std::move(measured_position) }
                        });
                    }
                }
            }

            auto operators = nlohmann::json::array();
            for (const auto index : metadata_by_observation.at(i)->operator_observations) {
                const auto & operation = graph.runtime_observations()[index];
                const auto operation_position = measurement_position(operation);
                if (!operation_position) continue;

                auto descriptor = nlohmann::json{
                    { "position", *operation_position },
                    { "operator",      operation.name }
                };
                if (operation.has_arg("Input Dims")) {
                    descriptor["input_dims_raw"] = operation.arg("Input Dims");
                    descriptor["input_dimensions"] = input_dimensions(operation.arg("Input Dims"));
                }
                operators.push_back(std::move(descriptor));
            }
            // Descriptor values retain the trace reader's textual argument
            // representation. This is source evidence, not target geometry
            // or another additive cost for the enclosed Torch operators.
            metadata_calls.push_back({
                {           "position",                      *position },
                {         "source_pid",                      event.pid },
                {    "descriptor_args",               event.args_map() },
                {   "caller_ownership",           region.caller.status },
                {     "caller_overlap", region.caller.has_node_overlap },
                {  "submitted_devices",             std::move(devices) },
                {     "phase_position",      std::move(phase_position) },
                {   "metadata_program",    std::move(metadata_program) },
                { "operator_envelopes",           std::move(operators) }
            });
            continue;
        }

        const auto raw = event.arg("Input Dims");
        const auto dimensions = input_dimensions(raw);
        parsed += !dimensions.is_null();
        // Process identity is source-local. A Torch event does not itself
        // establish the manifest's logical input/rank correspondence.
        calls.push_back({
            {                    "position",                      *position },
            {                    "operator",                     event.name },
            {                  "source_pid",                      event.pid },
            {              "input_dims_raw",                            raw },
            {            "input_dimensions",                     dimensions },
            {            "caller_ownership",           region.caller.status },
            {              "caller_overlap", region.caller.has_node_overlap },
            {           "submitted_devices",             std::move(devices) },
            { "uniquely_owned_device_nodes",        nlohmann::json::array() }
        });
    }

    size_t unique_devices = 0;
    for (const auto & [node, candidates] : device_envelopes) {
        // Choose only a strict innermost envelope on one caller thread.
        // Equal intervals and non-nested ownership remain ambiguous. Neither
        // duration proximity nor a familiar kernel name breaks a tie.
        const auto owner = std::ranges::find_if(candidates, [&](const Envelope & candidate) {
            return std::ranges::all_of(candidates, [&](const Envelope & other) {
                if (candidate.row == other.row) return true;
                return candidate.event->pid == other.event->pid && candidate.event->tid == other.event->tid && candidate.begin_ns >= other.begin_ns
                       && candidate.end_ns <= other.end_ns && (candidate.begin_ns > other.begin_ns || candidate.end_ns < other.end_ns);
            });
        });
        if (owner == candidates.end()) continue;

        calls[owner->row]["uniquely_owned_device_nodes"].push_back(node);
        ++unique_devices;
    }

    return {
        { "scope", "shape-bearing source Torch envelopes and their submitted device work; not target shapes or training features" },
        { "shape_envelopes", calls.size() },
        { "parsed_shape_envelopes", parsed },
        { "unparsed_shape_envelopes", calls.size() - parsed },
        { "linked_device_nodes", device_envelopes.size() },
        { "multiply_owned_device_nodes", std::ranges::count_if(device_envelopes, [](const auto & row) { return row.second.size() > 1; }) },
        { "uniquely_attributed_device_nodes", unique_devices },
        { "ambiguous_device_nodes", device_envelopes.size() - unique_devices },
        { "full_geometry_coverage_proven", false },
        { "limitation",
         "Unique ownership is among retained shape-bearing envelopes only; missing envelopes, parsed dimensions, layout, dtype and target geometry remain "
          "separate requirements." },
        { "calls", std::move(calls) },
        { "attention_metadata_calls", std::move(metadata_calls) },
    };
}

} // namespace markov::trace_graph::source_dag
