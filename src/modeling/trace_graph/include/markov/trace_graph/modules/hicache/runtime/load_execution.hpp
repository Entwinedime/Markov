#pragma once
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <span>
#include <string_view>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

struct LoadIndexOperationCost {
    uint64_t main_us = 0, main_residual_us = 0;
    uint64_t worker_us = 0, dispatch_us = 0, device_us = 0;
};

struct LoadIndexResources {
    size_t worker, compute;
};
[[nodiscard]] HiCacheHostExpansion generate_layer_wait(const core::DagGraph & graph, size_t main, size_t worker, size_t compute,
                                                       const LoadIndexOperationCost & cost, uint64_t after_us, uint64_t after_residual_us);
struct LoadSubmissionCost {
    LoadIndexOperationCost before_sync, start_record, wait_event;
    LoadIndexOperationCost first_copy, copy, first_layer_record, layer_record, tail;
};
/** Partition a complete Ascend submission into per-operation costs. Unsupported
 * layouts return no observation; CPU residuals and dispatch remain separate. */
[[nodiscard]] std::optional<LoadSubmissionCost> observe_load_submission_cost(const HiCacheWriteExpansion & plan, size_t layers);
struct GeneratedLoadSubmission {
    HiCacheWriteExpansion plan;
    std::vector<size_t> layer_records;
};
/** Ascend page_first_direct: index D2H waits for compute, then K/V copies
 * transfer all layers before layer completion records. Structure depends only
 * on target pages/layers; costs and resource identities are separate inputs. */
[[nodiscard]] GeneratedLoadSubmission generate_load_submission(const core::DagGraph & graph, size_t main, size_t worker, size_t compute, size_t load_stream,
                                                               size_t pages, uint64_t page_bytes, size_t layers, const LoadSubmissionCost & cost);
/** Bind a worker FIFO to an explicitly identified compute stream using this
 * main lane's submission -> worker -> device evidence. The target operation
 * itself need not have occurred in base; the caller supplies the compute role. */
[[nodiscard]] std::optional<LoadIndexResources> observe_load_index_resources(const patch::HiCacheSourceDagIndex & source,
                                                                             const simulation::detail::CpuTaskQueues & queues, size_t main_node,
                                                                             size_t compute_node);

/** One asynchronously submitted index operation, not a copied calibration DAG.
 * Resource nodes identify a proven main lane, existing worker FIFO and compute
 * stream. Cost includes main preparation/submission, worker service and device
 * service separately. The host return does not wait for device completion. */
[[nodiscard]] HiCacheHostExpansion generate_load_index_operation(const core::DagGraph & graph, size_t main_node, size_t worker_node, size_t compute_node,
                                                                 const LoadIndexOperationCost & cost, std::string_view operation);

/** Generate the ordered device portion of a layer-first H2D batch. The caller
 * supplies target layer bytes and the independently estimated total service.
 * No source trace or transfer template is required. Optional existing records
 * retain their submission dependencies; otherwise new completion records are
 * emitted. Existing byte attribution may assign zero weight to an intermediate
 * record; that record and its ordering still remain. The total must be positive.
 * CPU launch work, initial stream ordering and downstream stream tails
 * remain the caller's responsibility, not an implicit device-wide barrier. */
[[nodiscard]] std::vector<size_t> generate_load_layer_transfers(simulation::FutureDag & future, size_t ready, std::string_view lane,
                                                                std::span<const uint64_t> layer_bytes, uint64_t service_us,
                                                                std::span<const size_t> completion_records = {});

} // namespace markov::trace_graph::modules::hicache::runtime
