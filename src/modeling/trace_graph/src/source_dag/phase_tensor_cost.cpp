#include "markov/trace_graph/modules/hicache/layer_waits.hpp"
#include "markov/trace_graph/source_dag/call_regions.hpp"
#include "markov/trace_graph/source_dag/observations.hpp"
#include "markov/trace_graph/source_dag/tensor_cost.hpp"
#include "markov/trace_graph/structure/hicache/attention.hpp"
#include "markov/trace_graph/structure/tensor_program.hpp"

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>

namespace markov::trace_graph::source_dag {

nlohmann::json phase_tensor_operator_observations(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                  const simulation::detail::CpuTaskQueues & queues,
                                                  const modules::hicache::HiCachePhaseObservationAudit & phases, bool cpu_service_applied) {
    const auto & graph = source.graph();
    nlohmann::json rows = nlohmann::json::array();
    const auto calls = observe_attention_calls(source, queues, phases);
    const auto layer_waits = modules::hicache::observe_hicache_layer_waits(source);
    std::set<size_t> measured_devices;

    for (const auto & call : calls.calls) {
        const auto program = structure::hicache::ascend_attention_tensor_program(call.conditions, call.batch, call.cache_write);
        if (!program) continue;

        const auto matched = match_tensor_operators(source, queues, call.region.operator_observations, *program);
        append_tensor_program_observations(source, queues, *program, matched, measured_devices, cpu_service_applied, rows);
        for (const auto & row : tensor_program_outer_observations(source, call.region.work, *program, matched, cpu_service_applied, layer_waits.calls))
            rows.push_back(row);
    }

    // Metadata is outside per-layer attention. Match each enclosed operator
    // and the intervening CPU intervals, never count the outer wall time as
    // an additional service or attach layer costs a second time.
    const auto positions = phase_call_positions(phases);
    for (const auto & region : observe_attention_metadata_regions(source, queues)) {
        if (region.work.caller.status != "ready" || region.work.caller.has_node_overlap) continue;
        const auto owner = call_phase_position(region.work, positions);
        if (!owner) continue;

        const auto & descriptor = graph.runtime_observations().at(region.work.observation);
        const auto inputs = observed_attention_metadata_inputs(descriptor);
        if (!inputs) continue;
        const auto program = structure::hicache::ascend_attention_metadata_program(owner->decode_iteration ? "decode" : "prefill", *inputs);
        if (!program) continue;

        const auto matched = match_tensor_operators(source, queues, region.operator_observations, *program);
        append_tensor_program_observations(source, queues, *program, matched, measured_devices, cpu_service_applied, rows);
        for (const auto & row : tensor_program_outer_observations(source, region.work, *program, matched, cpu_service_applied, layer_waits.calls))
            rows.push_back(row);
    }
    // Source-structure reporting owns admission diagnostics; only admitted
    // measurements enter this cost export.
    std::map<std::string, size_t> dense_issues;
    for (const auto & region : observe_dense_regions(source, queues, dense_issues)) {
        const auto owner = call_phase_position(region.work, positions);
        if (!owner) continue;
        const std::string component = owner->decode_iteration ? "decode" : "prefill";
        const auto program = structure::dense_tensor_program(component, region.implementation, region.shape);
        const auto matched = match_tensor_operators(source, queues, region.operator_observations, program);

        const bool cold_preparation = std::ranges::any_of(region.operator_observations, [&](size_t index) {
            const auto & event = graph.runtime_observations().at(index);
            return event.name == "runtime.triton.prepare" || event.name == "runtime.triton.load";
        });
        // Compilation/loading is not steady-state caller service. Device and
        // submitted worker scopes remain separately measurable on that call.
        append_tensor_program_observations(source, queues, program, matched, measured_devices, cpu_service_applied, rows, !cold_preparation);
        if (!cold_preparation)
            for (const auto & row : tensor_program_outer_observations(source, region.work, program, matched, cpu_service_applied, layer_waits.calls))
                rows.push_back(row);
    }
    return rows;
}

} // namespace markov::trace_graph::source_dag
