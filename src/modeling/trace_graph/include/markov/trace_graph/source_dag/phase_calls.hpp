#pragma once

#include "markov/trace_graph/source_dag/call_regions.hpp"
#include "markov/trace_graph/source_dag/device_collectives.hpp"

#include <cstddef>
#include <map>
#include <string>

namespace markov::trace_graph::source_dag {

/** Immutable source calls joined to the same phase observation audit.
 * Dense call indices address attention.calls; phase indices address that audit.
 * Observation issues retain their family and are combined only on request.
 * No target work, costs or graph mutation are produced here.
 */
struct PhaseCallAudit {
    AttentionCallAudit attention;
    AttentionMetadataAudit metadata;
    DenseCallAudit dense;
    DeviceCollectiveAudit collectives;

    [[nodiscard]] std::map<std::string, size_t> issues() const;
};

/** Observe all phase families before source replacement.
 * The index and queues must describe the same immutable source graph.
 */
[[nodiscard]] PhaseCallAudit observe_phase_calls(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                 const simulation::detail::CpuTaskQueues & queues,
                                                 const modules::hicache::HiCachePhaseObservationAudit & phases);

} // namespace markov::trace_graph::source_dag
