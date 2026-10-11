#pragma once

#include "markov/trace_graph/cost/host_operation.hpp"
#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/structure/tensor_program.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

struct TensorOperationSlots {
    struct DeviceLaunch {
        size_t worker, device;
        structure::OperationWork work;
    };
    size_t submit;
    std::optional<size_t> worker, device, tail;
    std::optional<size_t> worker_return;
    size_t host_return;
    structure::OperationWork work;
    std::string tail_operation;
    std::string tail_scope = "cpu_service";
    std::vector<DeviceLaunch> device_launches;
    std::optional<structure::OperationWork> device_work;
};

/** Cost-free lowering on one compute stream and worker FIFO, with target
 * HiCache layer checks. has_layer_load comes from target cache execution, not
 * source wait durations or cost availability.
 * Each device slot represents a tensor operation's serial device service, not
 * a claim that the framework emits exactly one native kernel for that op.
 * All numeric slots must be bound before expansion. A synchronous readback
 * first waits for compute, then holds the caller through its copy/return;
 * it has no invented worker or asynchronous device-service slot. Direct
 * blocking kernel launches separately wait after their device work.
 * Ordinary submissions and layer event waits remain asynchronous.
 * Explicit host-only prerequisites are honored as well: device producers
 * synchronize their consumer without moving the wait before readback setup.
 * Host list conversion belongs to score preparation. Tensor return work has its own CPU
 * slot after submission, overlapping device work unless the call blocks.
 * Outer CPU work and boundary rewiring are separate.
 * Ordinary tensor workers have a launch part and a return part in one FIFO
 * task. Device work starts after launch; the queue releases after return.
 * Explicit backend child launches use one worker task with separate launch
 * segments and ordered devices, for attention and allocator alike. Multiple
 * independent tasks still require their own lowering; unknown native work
 * cannot train the single-launch scopes.
 */
struct GeneratedTensorProgram {
    HiCacheHostExpansion plan;
    std::vector<TensorOperationSlots> operations;
    // In plan.event_waits order. The caller supplies each target layer's
    // completion node to expand_host; no source event is copied here.
    std::vector<uint64_t> event_layers;
};

/** Lower the shared work description without translating operation types.
 * identity_prefix names report attributes, not a cost or execution policy.
 * Allocator callers leave has_layer_load false; phase callers provide the
 * target readiness condition. Optional request_index remains report metadata.
 */
[[nodiscard]] GeneratedTensorProgram generate_tensor_program(const core::DagGraph & graph, size_t main, size_t worker, size_t compute,
                                                             std::span<const structure::TensorOperation> operations, const std::string & identity_prefix,
                                                             bool has_layer_load = false);

/** Describe only the service scopes selected by the structural slots. Active
 * waits use the same submit/tail families as standalone HiCache layer calls;
 * inactive returns do not require worker/device evidence.
 * Blocking returns request a single post-completion interval. This projection
 * removes all graph identities; it makes no model queries or numeric choices.
 */
[[nodiscard]] std::vector<cost::TensorCostRequest> tensor_cost_requests(std::span<const TensorOperationSlots> operations);

/** Bind one already-estimated numeric record per operation, in program order.
 * Modifies only numeric slots in the supplied program; retains event_layers.
 * No model queries, topology choices, source cost projection or graph writes.
 */
void bind_tensor_program(GeneratedTensorProgram & generated, std::span<const cost::TensorOperationCost> costs);

} // namespace markov::trace_graph::modules::hicache::runtime
