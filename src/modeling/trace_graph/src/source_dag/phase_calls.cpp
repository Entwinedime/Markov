#include "markov/trace_graph/source_dag/phase_calls.hpp"

#include <initializer_list>

namespace markov::trace_graph::source_dag {

std::map<std::string, size_t> PhaseCallAudit::issues() const {
    auto result = attention.issues;

    for (const auto * family : { &metadata.issues, &dense.issues, &collectives.issues })
        for (const auto & [issue, count] : *family) result[issue] += count;

    return result;
}

PhaseCallAudit observe_phase_calls(const modules::hicache::patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                   const modules::hicache::HiCachePhaseObservationAudit & phases) {
    PhaseCallAudit result;
    const auto positions = phase_call_positions(phases);

    result.attention = observe_attention_calls(source, queues, phases, positions);
    result.metadata = observe_attention_metadata_calls(source, queues, phases, positions);
    result.dense = observe_dense_calls(source, queues, positions, result.attention);
    result.collectives = observe_device_collectives(source, queues, phases, positions);

    return result;
}

} // namespace markov::trace_graph::source_dag
