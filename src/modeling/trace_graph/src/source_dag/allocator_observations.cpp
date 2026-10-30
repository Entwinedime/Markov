#include "markov/trace_graph/adapters/frameworks/sglang/source_calls.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/source_dag/index.hpp"
#include "markov/trace_graph/source_dag/observations.hpp"
#include "markov/trace_graph/source_dag/tensor_cost.hpp"
#include "source_dag/observation_writer.hpp"

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace markov::trace_graph::source_dag {

void allocator_operator_observations(const SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues, bool cpu_service_applied,
                                     ObservationWriter & rows) {
    std::set<size_t> measured_devices;
    for (const auto & call : adapters::sglang::observe_allocator_programs(source, queues)) {
        if (!call.program) continue;
        const auto & program = *call.program;
        const auto & matched = call.matched_operators;
        // Cold launch CPU includes separately measured JIT service. Omit only
        // that caller's scopes; attributed device/worker service remains usable.
        append_tensor_program_observations(source,
                                           queues,
                                           program,
                                           matched,
                                           measured_devices,
                                           cpu_service_applied,
                                           rows,
                                           true,
                                           call.has_preparation ? call.fused_position : std::nullopt);

        // Admit outer CPU samples only when every tensor boundary is paired.
        // Otherwise missing tensor work could leak into a preparation sample.
        if (call.has_preparation || !call.complete_tensor_mapping) continue;
        for (const auto & row : tensor_program_outer_observations(source, call.region.work, program, matched, cpu_service_applied)) rows.push_back(row);
    }
}

} // namespace markov::trace_graph::source_dag
