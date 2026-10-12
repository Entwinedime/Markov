#pragma once
#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/cost/hicache_release.hpp"
#include "markov/trace_graph/cost/hicache_submission.hpp"
#include "markov/trace_graph/cost/host_operation.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

struct LoadIndexResources {
    size_t worker, compute;
};

/** Local numeric slots for one declared host operation. These identities do
 * not contain costs; binding may change durations, never topology or resources. */
struct HostOperationSlots {
    size_t main;
    std::optional<size_t> worker, device;
    std::optional<size_t> worker_return;
};

/** Bind a worker segment's service/residual interval in microseconds.
 * Dispatch is separate and belongs only to the task head. Keeps topology,
 * queue identity and existing dispatch metadata unchanged. */
void bind_worker_segment(core::DagSyntheticNodeSpec & worker, uint64_t service_us, uint64_t residual_us, std::string_view source);

/** Bind microsecond service values to declared slots; absent worker/device
 * roles stay absent. Does not add operations or change their dependencies. */
void bind_host_operation(HiCacheHostExpansion & plan, const HostOperationSlots & slots, const cost::HostOperationCost & cost,
                         std::string_view source = "hicache_host_independent_model");

enum class ReleaseOperation { BeforeFirstSync, FirstSync, BeforeSecondSync, SecondSync, Concatenate, Tail };
struct GeneratedPagedRelease {
    HiCacheHostExpansion plan;
    std::vector<std::pair<ReleaseOperation, HostOperationSlots>> operations;
};
/** Current Ascend paged immediate-free lowering: host fallback index work
 * synchronizes compute twice, then asynchronously concatenates free pages.
 * Counts and ordering belong to this lowering, not to a calibration sample.
 */
[[nodiscard]] GeneratedPagedRelease generate_paged_release(const core::DagGraph & graph, size_t main, size_t worker, size_t compute, bool paged = true);
void bind_paged_release(GeneratedPagedRelease & generated, const cost::PagedReleaseCost & cost);
/** A stream-wait program generated solely from resource identities. Semantic
 * cost slots let the later cost stage bind values without changing topology.
 */
struct GeneratedLayerWait {
    HiCacheHostExpansion plan;
    size_t submit = 0, worker = 0, device = 0, tail = 0, worker_return = 0;
};
[[nodiscard]] GeneratedLayerWait generate_layer_wait(const core::DagGraph & graph, size_t main, size_t worker, size_t compute);
enum class TransferOperation { PrepareSync, IndexSync, StartRecord, WaitEvent, FirstCopy, Copy, FirstCompletionRecord, CompletionRecord, Tail };
struct GeneratedTransferSubmission {
    HiCacheWriteExpansion plan;
    std::vector<size_t> completion_records;
    std::vector<std::pair<TransferOperation, HostOperationSlots>> operations;
};
/** Ascend page_first_direct: index D2H waits for compute, then ordered K/V
 * copies followed by completion records. Framework rules supply one global
 * record for a write, or one per layer for a load; measurements cannot choose
 * this count or the stream/event dependencies. Payload service is bound later.
 */
[[nodiscard]] GeneratedTransferSubmission generate_page_first_submission(const core::DagGraph & graph, size_t main, size_t worker, size_t compute,
                                                                         size_t transfer_stream, size_t pages, uint64_t page_bytes, size_t completion_count,
                                                                         std::string_view operation);
/** Fill control service slots only. Payload I/O remains bound separately;
 * page count, completion count, waits and resource lanes are already fixed. */
void bind_page_first_submission(GeneratedTransferSubmission & generated, const cost::PageFirstSubmissionCost & cost);
/** Bind a worker FIFO to an explicitly identified compute stream using this
 * main lane's submission -> worker -> device evidence. The target operation
 * itself need not have occurred in base; the caller supplies the compute role. */
[[nodiscard]] std::optional<LoadIndexResources> observe_load_index_resources(const patch::HiCacheSourceDagIndex & source,
                                                                             const simulation::detail::CpuTaskQueues & queues, size_t main_node,
                                                                             size_t compute_node);

/** One asynchronously submitted index operation, not a copied calibration DAG.
 * Resource nodes identify a proven main lane, existing worker FIFO and compute
 * stream. Numeric slots are bound later; the host return does not wait for
 * device completion. Construction never queries or selects a cost model. */
struct GeneratedDeviceOperation {
    HiCacheHostExpansion plan;
    size_t submit = 0, worker = 0, device = 0, worker_return = 0;
};
[[nodiscard]] GeneratedDeviceOperation generate_async_device_operation(const core::DagGraph & graph, size_t main_node, size_t worker_node, size_t compute_node,
                                                                       std::string_view operation);

} // namespace markov::trace_graph::modules::hicache::runtime
