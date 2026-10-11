#pragma once

#include "markov/trace_graph/cost/host_operation.hpp"
#include "markov/trace_graph/cost/operator_model.hpp"
#include <cstddef>
#include <cstdint>

namespace markov::trace_graph::cost {

struct PageFirstSubmissionCost {
    HostOperationCost prepare_sync, index_sync, start_record, wait_event;
    HostOperationCost first_copy, copy, first_completion_record, completion_record, tail;
};

/** Independent submission service, excluding payload I/O and synchronization
 * waiting. All scopes have their own model; missing service is not zero.
 * The structure generator, not this provider, owns page/completion counts.
 */
[[nodiscard]] PageFirstSubmissionCost estimate_write_submission(OperatorCostProvider & provider, uint64_t payload_bytes, uint64_t page_bytes, bool write_back);

[[nodiscard]] PageFirstSubmissionCost estimate_load_submission(OperatorCostProvider & provider, uint64_t payload_bytes, uint64_t page_bytes, size_t layers);

} // namespace markov::trace_graph::cost
