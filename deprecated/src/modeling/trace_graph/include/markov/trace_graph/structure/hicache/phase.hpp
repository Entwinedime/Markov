#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/modules/hicache/model/phase_work.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include "markov/trace_graph/source_dag/call_regions.hpp"
#include "markov/trace_graph/source_dag/device_collectives.hpp"
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
    enum class Kind { Attention, Metadata, Dense };
    size_t source_observation;
    // Semantic correspondence for experiment reports, never cost features.
    int logical_input;
    std::string request_id;
    std::optional<uint64_t> decode_iteration, layer_index;
    Action action = Action::Unavailable;
    std::vector<TensorOperation> operations;
    std::string reason;
    Kind kind = Kind::Attention;
    std::string dense_role;
};

/** Plan one source-corresponding call after target admission has fixed its
 * token/cache work. No graph modification, cost query or target trace access.
 */
[[nodiscard]] PhaseCallPlan plan_attention_call(const source_dag::AttentionCall & source, const modules::hicache::model::HiCachePrefillWorkItem & prefill,
                                                const modules::hicache::model::HiCacheDecodeWorkItem & decode);

[[nodiscard]] PhaseCallPlan plan_attention_metadata_call(const source_dag::AttentionMetadataCall & source,
                                                         const modules::hicache::model::HiCachePrefillWorkItem & prefill,
                                                         const modules::hicache::model::HiCacheDecodeWorkItem & decode);

[[nodiscard]] PhaseCallPlan plan_dense_call(const source_dag::DenseCall & source, const source_dag::AttentionCall & attention,
                                            const modules::hicache::model::HiCachePrefillWorkItem & prefill);

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

[[nodiscard]] DeviceCollectivePlan plan_device_collective_call(const source_dag::DeviceCollectiveCall & source,
                                                               std::span<const source_dag::AttentionCall> attention,
                                                               const modules::hicache::model::HiCachePrefillWorkItem & prefill);

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
