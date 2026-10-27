#pragma once

#include "markov/trace_graph/structure/operation.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace markov::trace_graph::structure::hicache {

enum class CollectiveTaskKind { Record, Wait, Reduce, Gather, Destroy };

[[nodiscard]] std::string_view collective_task_name(CollectiveTaskKind kind);

/** Describe one queue primitive, not an entire collective or its measured
 * duration. Reduction submission depends on payload; event tasks operate on
 * one event and do not acquire token/byte features from their enclosing call.
 * Source export and target binding share this operation/feature contract. */
[[nodiscard]] OperationWork collective_task_work(const OperationWork & collective, CollectiveTaskKind kind);

struct CollectiveCallerStep {
    std::string_view name;
    std::optional<CollectiveTaskKind> task;
};

/** Ordinary queued eager all-reduce or all-gather: input-stream handoff, transfer,
 * future/work completion events, output-stream handoff, and future cleanup.
 * This is an execution rule, not a source task-count template. Other debug,
 * capture or lifetime branches need their own declared rule. */
[[nodiscard]] std::span<const CollectiveCallerStep> ordinary_collective_caller_steps(CollectiveTaskKind transfer);

/** Caller metadata preparation has one packed 2D tensor, not an elementwise
 * CPU loop over its payload. Byte-dependent HCCL work belongs to the worker.
 * The named step distinguishes event creation/registration from enqueue. */
[[nodiscard]] OperationWork collective_caller_work(const OperationWork & collective, const CollectiveCallerStep & step);

} // namespace markov::trace_graph::structure::hicache
