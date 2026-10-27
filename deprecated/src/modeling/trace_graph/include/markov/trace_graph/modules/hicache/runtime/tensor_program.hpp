#pragma once

#include "markov/trace_graph/modules/hicache/runtime/host_program.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/structure/tensor_program.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

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
    std::vector<HostOperationBinding> operations;
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

} // namespace markov::trace_graph::modules::hicache::runtime
