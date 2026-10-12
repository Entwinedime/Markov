#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/layer_waits.hpp"
#include "markov/trace_graph/source_dag/call_regions.hpp"
#include "markov/trace_graph/source_dag/observations.hpp"
#include "markov/trace_graph/source_dag/tensor_cost.hpp"
#include "markov/trace_graph/structure/hicache/attention.hpp"
#include "markov/trace_graph/structure/tensor_program.hpp"

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::source_dag {
namespace {


nlohmann::json outer_cpu_observations(const modules::hicache::patch::HiCacheSourceDagIndex & source, const SubmittedCallRegion & region,
                                      std::span<const structure::TensorOperation> program, const std::map<size_t, size_t> & matched,
                                      const modules::hicache::HiCacheLayerWaitObservation & layer_waits, bool corrected) {
    const auto & graph = source.graph();
    const auto & outer = graph.runtime_observations().at(region.observation);
    const auto position = measurement_position(outer);
    nlohmann::json rows = nlohmann::json::array();
    if (!position) return rows;
    // An operation rooted at its own Torch envelope already exports caller
    // scopes. Only framework programs with an explicit CPU tail have outer
    // preparation/return intervals; do not export the whole inner call again.
    if (program.empty() || program.back().execution != structure::TensorExecutionKind::CpuService) return rows;

    const auto outer_end = core::checked_add_u64(outer.ts, outer.dur, "attention end overflow");
    std::vector<const modules::hicache::HiCacheLayerWaitCall *> nested;
    for (const auto & wait : layer_waits.calls) {
        if (!wait.issue.empty() || !wait.before) continue;
        const auto & event = graph.event_for_node(*wait.before);
        if (event.pid == outer.pid && event.tid == outer.tid && wait.start_ns / 1'000 >= outer.ts && wait.end_ns / 1'000 <= outer_end) nested.push_back(&wait);
    }
    std::ranges::stable_sort(nested, [](const auto * left, const auto * right) {
        return std::pair{ left->start_ns, left->position } < std::pair{ right->start_ns, right->position };
    });

    const auto append = [&](const structure::OperationWork & work, uint64_t begin, uint64_t end) {
        return append_cpu_interval_observations(source, outer, work, begin, end, corrected, rows);
    };

    // The framework program alternates CPU preparation and its consumer,
    // followed by the outer return. All boundaries must match before any
    // samples are admitted; partial matching could charge omitted work twice.
    uint64_t previous = outer.ts;
    size_t layer = 0;
    for (size_t i = 0; i + 1 < program.size(); i += 2) {
        const auto & operation = program[i + 1];
        uint64_t begin, end;
        if (operation.execution == structure::TensorExecutionKind::LayerReady) {
            if (layer == nested.size() || nested[layer]->layer != operation.layer_index) return nlohmann::json::array();
            begin = nested[layer]->start_ns / 1'000;
            end = nested[layer++]->end_ns / 1'000;
        }
        else {
            const auto found = matched.find(i + 1);
            if (found == matched.end()) return nlohmann::json::array();
            const auto & event = graph.runtime_observations().at(found->second);
            begin = event.ts;
            end = core::checked_add_u64(begin, event.dur, "attention operator end overflow");
        }
        if (begin < previous || end > outer_end || !append(program[i].work, previous, begin)) return nlohmann::json::array();
        previous = end;
    }
    if (layer != nested.size() || !append(program.back().work, previous, outer_end)) return nlohmann::json::array();
    return rows;
}

} // namespace

nlohmann::json phase_tensor_operator_observations(const core::DagGraph & graph, const modules::hicache::HiCachePhaseObservationAudit & phases,
                                                  bool cpu_service_applied) {
    nlohmann::json rows = nlohmann::json::array();
    const modules::hicache::patch::HiCacheSourceDagIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    const auto calls = observe_attention_calls(source, queues, phases);
    const auto layer_waits = modules::hicache::observe_hicache_layer_waits(source);
    std::set<size_t> measured_devices;

    for (const auto & call : calls.calls) {
        const auto program = structure::hicache::ascend_attention_tensor_program(call.conditions, call.batch, call.cache_write);
        if (!program) continue;

        const auto matched = match_tensor_operators(source, queues, call.region.operator_observations, *program);
        for (const auto & [work_index, index] : matched)
            for (const auto & row : tensor_operator_observations(source,
                                                                 queues,
                                                                 program->at(work_index).work,
                                                                 index,
                                                                 measured_devices,
                                                                 cpu_service_applied,
                                                                 true,
                                                                 program->at(work_index).device_launches))
                rows.push_back(row);
        for (const auto & row : outer_cpu_observations(source, call.region.work, *program, matched, layer_waits, cpu_service_applied)) rows.push_back(row);
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
        for (const auto & [work_index, index] : matched)
            for (const auto & row : tensor_operator_observations(source,
                                                                 queues,
                                                                 program->at(work_index).work,
                                                                 index,
                                                                 measured_devices,
                                                                 cpu_service_applied,
                                                                 true,
                                                                 program->at(work_index).device_launches))
                rows.push_back(row);
        for (const auto & row : outer_cpu_observations(source, region.work, *program, matched, layer_waits, cpu_service_applied)) rows.push_back(row);
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
        for (const auto & [work_index, observation] : matched)
            for (const auto & row : tensor_operator_observations(source,
                                                                 queues,
                                                                 program[work_index].work,
                                                                 observation,
                                                                 measured_devices,
                                                                 cpu_service_applied,
                                                                 !cold_preparation,
                                                                 program[work_index].device_launches))
                rows.push_back(row);
        if (!cold_preparation)
            for (const auto & row : outer_cpu_observations(source, region.work, program, matched, layer_waits, cpu_service_applied)) rows.push_back(row);
    }
    return rows;
}

} // namespace markov::trace_graph::source_dag
