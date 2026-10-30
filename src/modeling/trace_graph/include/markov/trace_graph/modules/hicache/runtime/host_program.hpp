#pragma once
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/cost/host_operation.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

struct HostDeviceResources {
    size_t worker, compute;
};

/** Local numeric slots for one declared host operation. These identities do
 * not contain costs; binding may change durations, never topology or resources.
 * A CPU-only task uses worker without device/worker_return. Multiple device
 * launches each have their own worker segment; they replace the parent device
 * charge, not the caller, task-return or optional caller-tail scopes. */
struct HostOperationSlots {
    struct DeviceLaunch {
        size_t worker, device;
    };
    size_t main;
    std::optional<size_t> worker, device;
    std::optional<size_t> worker_return;
    std::optional<size_t> tail;
    std::vector<DeviceLaunch> device_launches;
    uint64_t main_repetitions = 1;
};

/** Bind a worker segment's service/residual interval in microseconds.
 * Dispatch is separate and belongs only to the task head. Keeps topology,
 * queue identity and existing dispatch metadata unchanged. */
void bind_worker_segment(core::DagSyntheticNodeSpec & worker, uint64_t service_us, uint64_t residual_us, std::string_view source);

/** Bind microsecond service values to declared slots; absent worker/device
 * roles stay absent. Does not add operations or change their dependencies. */
void bind_host_operation(HiCacheHostExpansion & plan, const HostOperationSlots & slots, const cost::HostOperationCost & cost,
                         std::string_view source = "hicache_host_independent_model");

/** Work declared by a generator and its numeric binding positions. Repeated
 * copies/records share one request only when their entire work is identical.
 * Cost evaluation consumes this declaration; it does not reconstruct roles. */
struct HostOperationBinding {
    cost::OperationCostRequest request;
    std::vector<HostOperationSlots> slots;
};

/** Resolve all scopes before binding. Does not change the generated plan,
 * except numeric durations and their provenance; payload I/O stays separate.
 * Each request and its slots are generated together. Repeated slots receive
 * the same resolved response, but remain separate execution operations. */
void bind_host_operations(HiCacheHostExpansion & plan, std::span<const HostOperationBinding> operations, cost::OperatorCostProvider & provider,
                          std::string_view source = "hicache_host_independent_model");

struct GeneratedPagedRelease {
    HiCacheHostExpansion plan;
    std::vector<HostOperationBinding> operations;
};
/** Current Ascend paged immediate-free lowering: host fallback index work
 * synchronizes compute twice, then asynchronously concatenates free pages.
 * Counts and ordering belong to this lowering, not to a calibration sample.
 */
[[nodiscard]] GeneratedPagedRelease generate_paged_release(const core::DagGraph & graph, size_t main, size_t worker, size_t compute, uint64_t released_pages,
                                                           uint64_t page_size, uint64_t buffer_pages, bool backed_up, bool need_sort);
/** A stream-wait program generated solely from resource identities. Semantic
 * cost slots let the later cost stage bind values without changing topology.
 */
struct GeneratedLayerWait {
    HiCacheHostExpansion plan;
    size_t submit = 0, worker = 0, device = 0, tail = 0, worker_return = 0;
};
[[nodiscard]] GeneratedLayerWait generate_layer_wait(const core::DagGraph & graph, size_t main, size_t worker, size_t compute);
struct GeneratedTransferSubmission {
    HiCacheWriteExpansion plan;
    std::vector<size_t> completion_records;
    std::vector<HostOperationBinding> operations;
};
/** Ascend page_first_direct: index D2H waits for compute, then ordered K/V
 * copies followed by completion records. Framework rules supply one global
 * record for a write, or one per layer for a load; measurements cannot choose
 * this count or the stream/event dependencies. Payload service is bound later.
 */
[[nodiscard]] GeneratedTransferSubmission generate_page_first_submission(const core::DagGraph & graph, size_t main, size_t worker, size_t compute,
                                                                         size_t transfer_stream, size_t pages, uint64_t page_bytes, size_t completion_count,
                                                                         std::string_view operation, bool write_back = false, bool compact_copies = true);
/** Bind a worker FIFO to an explicitly identified compute stream using this
 * main lane's submission -> worker -> device evidence. The target operation
 * itself need not have occurred in base; the caller supplies the compute role. */
[[nodiscard]] std::optional<HostDeviceResources> observe_host_device_resources(const adapters::sglang::HiCacheSourceIndex & source,
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
