#pragma once

#include "markov/trace_graph/cost/host_operation.hpp"
#include "markov/trace_graph/cost/operator_model.hpp"
#include <cstdint>

namespace markov::trace_graph::cost {

struct PagedReleaseCost {
    HostOperationCost before_first_sync, first_sync, before_second_sync, second_sync, concatenate, tail;
};

/** Costs for the supported Ascend immediate paged release lowering. The
 * concatenation buffer length comes from target allocator state, not base.
 * Synchronization waiting is represented by dependencies, not a fitted cost.
 */
[[nodiscard]] PagedReleaseCost estimate_paged_release(OperatorCostProvider & provider, uint64_t released_pages, uint64_t page_size, uint64_t buffer_pages,
                                                      bool backed_up, bool need_sort);

} // namespace markov::trace_graph::cost
