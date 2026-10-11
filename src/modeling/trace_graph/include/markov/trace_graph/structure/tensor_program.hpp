#pragma once

#include "markov/trace_graph/structure/operation.hpp"
#include "markov/trace_graph/structure/phase_shape.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace markov::trace_graph::structure {

enum class TensorExecutionKind { AsyncDevice, CpuService, BlockingContextReadback, DirectBlockingDevice, LayerReady };

[[nodiscard]] std::string_view tensor_execution_name(TensorExecutionKind kind);

/** Cost-free tensor/control work shared by phase and allocator submodules. Predecessors
 * address earlier operations in the same program and require their device
 * completion (or host return for host-only operations). For an asynchronous
 * consumer they constrain device execution, not CPU submission. For a host
 * consumer they constrain entry, or the copy after readback preparation.
 * LayerReady names a framework
 * readiness condition, not an observed wait duration or cost parameter. */
struct TensorOperation {
    OperationWork work;
    std::vector<size_t> predecessors;
    // Identifies a request within a multi-request program for reporting only.
    std::optional<size_t> request_index;
    TensorExecutionKind execution = TensorExecutionKind::AsyncDevice;
    std::optional<uint64_t> layer_index;
    // Explicit backend lowering within one queued call. Empty retains the
    // tensor-service abstraction; it does not prove a single native kernel.
    std::vector<OperationWork> device_launches;
};

/** Dense computation with measurable outer CPU boundaries: nonquantized
 * F.linear, fused SwiGLU, RMSNorm or residual-add RMSNorm.
 * Geometry/implementation are supplied by the framework rule or measurement;
 * this function neither chooses a target token layout nor reads costs. */
[[nodiscard]] std::vector<TensorOperation> dense_tensor_program(const std::string & component, const std::string & implementation, const DenseShape & shape);

} // namespace markov::trace_graph::structure
