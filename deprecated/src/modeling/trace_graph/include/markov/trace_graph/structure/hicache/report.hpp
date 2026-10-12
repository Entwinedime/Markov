#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/modules/hicache/model/effect_decision.hpp"
#include "markov/trace_graph/modules/hicache/model/phase_work.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include "markov/trace_graph/structure/hicache/phase.hpp"
#include <nlohmann/json_fwd.hpp>
#include <span>

namespace markov::trace_graph::structure::hicache {

/** Semantic work from the actual execution ledger, without cost or target data.
 * This projection describes effects and phase work, not a proof of emitted
 * edge correctness or complete operator-level correspondence across traces.
 */
[[nodiscard]] nlohmann::json effect_rows(const modules::hicache::model::HiCacheEffectDecisionLedger & ledger);
[[nodiscard]] nlohmann::json execution_structure_report(const core::DagGraph & graph, const modules::hicache::model::HiCacheEffectDecisionLedger & effects,
                                                        const modules::hicache::model::HiCachePhaseWorkLedger & phases,
                                                        std::span<const DeviceCollectivePlan> collectives);

/** Observed single-request phase operators and projected precedence.
 * Uses the same semantic correspondence as the executed report, never costs.
 * Precedence is a sparse report-local DAG: the first identity-count slots name
 * operations; later slots are anonymous junctions. Unowned single-successor
 * chains are contracted without expanding all reachable operation pairs.
 * Lane-sharing partitions contain semantic operators,
 * not cross-trace lane ids or proof of physical/internal-path correspondence. */
[[nodiscard]] nlohmann::json observed_phase_structure_report(const core::DagGraph & graph, const modules::hicache::HiCachePhaseObservationAudit & phases);

} // namespace markov::trace_graph::structure::hicache
