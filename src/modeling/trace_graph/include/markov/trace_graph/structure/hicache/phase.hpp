#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/modules/hicache/model/phase_work.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include "markov/trace_graph/source_dag/phase_calls.hpp"
#include "markov/trace_graph/structure/hicache/collective.hpp"
#include "markov/trace_graph/structure/operation.hpp"
#include "markov/trace_graph/structure/tensor_program.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace markov::trace_graph::structure::hicache {

struct PhaseCallPlan {
    // Retaining unchanged compute never retains an observed HiCache wait:
    // layer readiness remains owned by the target HiCache execution rules.
    enum class Action { RetainCompute, Generate, Unavailable };
    size_t source_observation;
    // Semantic correspondence for experiment reports, never cost features.
    int logical_input;
    std::string request_id;
    std::optional<uint64_t> decode_iteration, layer_index;
    Action action = Action::Unavailable;
    std::vector<TensorOperation> operations;
    std::string reason;
    // Semantic report label, not an execution or cost dispatch key.
    std::string call_kind = "layer";
};

/** One rank's logical all-reduce service demand. Peer observations locate
 * the corresponding call boundaries; they are not cost features or source
 * dependency templates. Runtime must join all target input-ready events
 * before service and reconnect each participant's result consumer.
 * A missing service is a structural gap, not a request for calibration. */
struct DeviceCollectivePlan {
    size_t source_observation;
    std::vector<size_t> peer_observations;
    std::optional<OperationWork> service;
    std::string reason;
};

struct PhaseCallWork {
    std::vector<PhaseCallPlan> tensors;
    std::vector<DeviceCollectivePlan> collectives;
};

/** Plan all calls for one admitted request/rank. source_phase is the exact
 * index established by request identity, not an execution-time guess.
 * Attention, metadata and dense retain their source order within each family;
 * collectives retain peer identities. Unavailable rules remain explicit plans.
 * No graph mutation, cost query or target trace access takes place here.
 */
[[nodiscard]] PhaseCallWork plan_phase_calls(const source_dag::PhaseCallAudit & source, size_t source_phase,
                                             const modules::hicache::model::HiCachePrefillWorkItem & prefill,
                                             const modules::hicache::model::HiCacheDecodeWorkItem & decode);

/** Establish migration candidates from immutable source identities for an
 * admitted request/rank. This is not work geometry for fitting a model.
 * Semantic owners remove replaced nodes; any remaining non-migratable node
 * requires a structural rule, irrespective of available fitted parameters.
 */
[[nodiscard]] std::map<size_t, CostMigration> phase_migration_candidates(const core::DagGraph & source_graph,
                                                                         const modules::hicache::HiCachePhaseObservation & source,
                                                                         const modules::hicache::model::HiCachePrefillWorkItem & prefill,
                                                                         const modules::hicache::model::HiCacheDecodeWorkItem & decode);

/** After structural owners remove replaced nodes, require unchanged work for
 * every remaining migration. Report missing rules before entering cost binding. */
void require_complete_phase_migrations(const std::map<size_t, CostMigration> & operations);

} // namespace markov::trace_graph::structure::hicache
