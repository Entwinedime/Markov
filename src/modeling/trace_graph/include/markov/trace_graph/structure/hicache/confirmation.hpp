#pragma once

#include "markov/trace_graph/structure/operation.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace markov::trace_graph::structure::hicache {

struct WriteConfirmationBatch {
    uint64_t backup_tokens, published_nodes;
};

struct WriteConfirmationStep {
    OperationWork work;
    bool destroys_event = false;
    std::optional<size_t> completed_batch;
};

/** SGLang storage-backed write confirmation; nonblocking starts after MIN.
 * Each current DMA batch owns two temporary events and one acknowledgement.
 * Reassigning the loop locals (or returning after the last batch) releases
 * those events. Blocking mode retains all batches until the queue is cleared
 * after the final acknowledgement, then releases the final loop-local pair.
 * Destruction submits CPU-only work; the caller never joins it.
 * Load confirmations are different: their events belong to the persistent
 * LayerDoneCounter pool and are not destroyed on acknowledgement.
 * No costs, source template or predicted timestamps enter this rule.
 */
[[nodiscard]] std::vector<WriteConfirmationStep> write_confirmation_program(std::span<const WriteConfirmationBatch> batches, bool blocking = false);

/** Backend task shared by event-destruction callers; no tensor/device work. */
[[nodiscard]] OperationWork event_destruction_work();

} // namespace markov::trace_graph::structure::hicache
