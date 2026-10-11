#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/runtime/preparation.hpp"
#include "markov/trace_graph/source_dag/observations.hpp"
#include "markov/trace_graph/source_dag/tensor_cost.hpp"

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace markov::trace_graph::source_dag {

std::vector<ObservedAllocatorProgram> observe_allocator_programs(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                                 const simulation::detail::CpuTaskQueues & queues) {
    const auto & graph = source.graph();
    std::vector<ObservedAllocatorProgram> calls;
    const auto preparations = modules::hicache::runtime::observe_allocator_preparations(graph);
    std::map<size_t, const modules::hicache::runtime::AllocatorPreparationSourceCall *> extend_calls;
    for (const auto & call : preparations.calls)
        if (call.allocator_observation) extend_calls.emplace(*call.allocator_observation, &call);
    std::map<std::pair<std::string, std::string>, std::vector<size_t>> launches;
    for (size_t i = 0; i < graph.runtime_observations().size(); ++i) {
        const auto & event = graph.runtime_observations()[i];
        if (event.name == "runtime.allocator.kernel" && event.arg("status") == "returned") launches[{ event.pid, event.tid }].push_back(i);
    }
    for (auto & [lane, indices] : launches) std::ranges::sort(indices, {}, [&](size_t index) { return graph.runtime_observations()[index].ts; });

    for (const auto & region : observe_allocator_regions(source, queues)) {
        calls.push_back({ .region = region });
        auto & result = calls.back();
        if (region.allocated != true) continue;
        const auto & event = graph.runtime_observations().at(region.work.observation);
        const auto deployment = observed_allocator_inputs(event);
        if (!deployment) continue;

        std::optional<std::vector<structure::TensorOperation>> program;
        bool has_preparation = false;
        if (region.phase == modules::hicache::model::HiCacheAllocatorPhase::Decode) {
            const auto work = observed_allocator_work(event);
            if (!work) continue;
            program = structure::hicache::ascend_decode_allocator_program(*work, *deployment);
        }
        else {
            const auto found = extend_calls.find(region.work.observation);
            if (found == extend_calls.end()) continue;
            const auto & call = *found->second;
            if (!call.issues.empty() || !call.allocator_work || !call.prefix_dtype || !call.page_count_supplied) continue;

            // Only this measured call supplies the sample's lengths, queue
            // branch and preparation. A one-call plan derives specialization
            // geometry; its inferred cache history is replaced by observation.
            auto shape = structure::hicache::plan_allocator_work({ event.pid }, { *call.allocator_work }).calls.front();
            if (shape.status == "naive_path") {
                program =
                    structure::hicache::ascend_naive_extend_allocator_program(*call.allocator_work, *deployment, *call.prefix_dtype, *call.page_count_supplied);
            }
            else {
                shape.status = call.required ? "required" : "already_prepared";
                shape.path = call.path;
                shape.first_load = call.first_load;
                has_preparation = !call.intervals.empty();
                program = structure::hicache::ascend_fused_extend_allocator_program(*call.allocator_work,
                                                                                    *deployment,
                                                                                    *call.prefix_dtype,
                                                                                    *call.page_count_supplied,
                                                                                    shape);
            }
        }
        if (!program) continue;

        std::map<size_t, std::string> schemas;
        bool complete_tensor_mapping = true;
        std::optional<size_t> fused_position;
        for (size_t i = 0; i < program->size(); ++i) {
            const auto & operation = program->at(i);
            if (operation.execution == structure::TensorExecutionKind::CpuService) continue;
            const auto & implementation = operation.work.implementation;
            if (implementation == "sgl_kernel_npu.alloc_extend_kernel:int64") {
                fused_position = i;
                continue;
            }
            if (!implementation.starts_with("aten.")) {
                // Triton launches need their own exact call observation.
                // Do not pair them to a nearby kernel or absorb their host
                // interval into the next aten preparation sample.
                complete_tensor_mapping = false;
                continue;
            }
            // The declared allocator program uses aten.<schema>:<dtype>.
            // Dtype is deployment/work identity, not part of the Torch name.
            schemas.emplace(i, "aten::" + implementation.substr(5, implementation.find(':') - 5));
        }
        auto matched = match_submitted_operator_envelopes(source, queues, region.operator_observations, schemas);
        if (fused_position) {
            std::vector<size_t> contained;
            const auto lane = launches.find({ event.pid, event.tid });
            const auto finish = core::checked_add_u64(event.ts, event.dur, "Allocator call end overflow");
            if (lane != launches.end()) {
                auto at = std::ranges::lower_bound(lane->second, event.ts, {}, [&](size_t index) { return graph.runtime_observations()[index].ts; });
                for (; at != lane->second.end(); ++at) {
                    const auto & launch = graph.runtime_observations()[*at];
                    if (launch.ts >= finish) break;
                    if (core::checked_add_u64(launch.ts, launch.dur, "Allocator kernel call end overflow") <= finish) contained.push_back(*at);
                }
            }
            if (contained.size() == 1) matched.emplace(*fused_position, contained.front());
            else complete_tensor_mapping = false;
        }
        result.program = std::move(program);
        result.matched_operators = std::move(matched);
        result.fused_position = fused_position;
        result.has_preparation = has_preparation;
        result.complete_tensor_mapping = complete_tensor_mapping && result.matched_operators.size() == schemas.size() + fused_position.has_value();
    }
    return calls;
}

nlohmann::json allocator_operator_observations(const core::DagGraph & graph, bool cpu_service_applied) {
    using Json = nlohmann::json;
    Json rows = Json::array();
    const modules::hicache::patch::HiCacheSourceDagIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    std::set<size_t> measured_devices;
    for (const auto & call : observe_allocator_programs(source, queues)) {
        if (!call.program) continue;
        const auto & program = *call.program;
        const auto & event = graph.runtime_observations().at(call.region.work.observation);
        const auto & matched = call.matched_operators;
        for (const auto & [position, observation] : matched) {
            // Cold launch CPU includes the separately measured JIT service.
            // Until those nested intervals are split, omit caller CPU scopes;
            // independently attributed device/worker service remains usable.
            const bool include_caller = !call.has_preparation || position != call.fused_position;
            for (const auto & row : tensor_operator_observations(source,
                                                                 queues,
                                                                 program.at(position).work,
                                                                 observation,
                                                                 measured_devices,
                                                                 cpu_service_applied,
                                                                 include_caller,
                                                                 program.at(position).device_launches))
                rows.push_back(row);
        }

        // Admit outer CPU samples only when every tensor boundary is paired.
        // Otherwise missing tensor work could leak into a preparation sample.
        if (call.has_preparation || !call.complete_tensor_mapping) continue;
        Json outer = Json::array();
        uint64_t previous = event.ts;
        const auto finish = core::checked_add_u64(event.ts, event.dur, "Allocator call end overflow");
        bool owned = true;
        for (size_t i = 0; i + 1 < program.size(); i += 2) {
            const auto & tensor = graph.runtime_observations().at(matched.at(i + 1));
            const auto end = core::checked_add_u64(tensor.ts, tensor.dur, "Allocator tensor end overflow");
            if (end > finish || !append_cpu_interval_observations(source, event, program.at(i).work, previous, tensor.ts, cpu_service_applied, outer)) {
                owned = false;
                break;
            }
            previous = end;
        }
        if (owned && append_cpu_interval_observations(source, event, program.back().work, previous, finish, cpu_service_applied, outer))
            for (const auto & row : outer) rows.push_back(row);
    }
    return rows;
}

} // namespace markov::trace_graph::source_dag
