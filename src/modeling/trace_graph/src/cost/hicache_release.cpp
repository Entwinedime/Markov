#include "markov/trace_graph/cost/hicache_release.hpp"
#include "markov/trace_graph/cost/host_operation.hpp"
#include <string>
#include <string_view>

namespace markov::trace_graph::cost {

PagedReleaseCost estimate_paged_release(OperatorCostProvider & provider, uint64_t released_pages, uint64_t page_size, uint64_t buffer_pages, bool backed_up,
                                        bool need_sort) {
    const auto implementation = std::string(page_size > 1 ? "ascend.paged_immediate." : "ascend.token_immediate.") + (backed_up ? "backup" : "regular");
    structure::OperationWork work{
        "hicache_direct",
        "",
        implementation,
        "",
        { { "released_pages", static_cast<double>(released_pages) },
          { "page_size", static_cast<double>(page_size) },
          { "release_buffer_pages", static_cast<double>(buffer_pages) },
          { "need_sort", need_sort ? 1.0 : 0.0 } }
    };
    const auto primitive = [&](std::string_view name, bool submitted) {
        work.operation = "release." + std::string(name);
        return estimate_host_operation(provider, work, submitted);
    };
    PagedReleaseCost result;
    if (page_size > 1) {
        result.before_first_sync = primitive("before_first_sync", false);
        result.before_second_sync = primitive("before_second_sync", false);
        structure::OperationWork synchronization{ .component = work.component,
                                                  .operation = "runtime.stream_sync",
                                                  .implementation = "ascend.runtime",
                                                  .timing_scope = "idle_stream_sync_interval",
                                                  .features = { { "previous_work_completed", 1.0 } } };
        const auto idle_us = provider.estimate(synchronization).duration_us;
        const auto sync_cost = [&](const char * operation) {
            work.operation = operation;
            work.timing_scope = "residual_interval";
            return HostOperationCost{ .main_us = idle_us, .main_residual_us = provider.estimate(work).duration_us };
        };
        result.first_sync = sync_cost("release.first_sync");
        result.second_sync = sync_cost("release.second_sync");
    }
    result.concatenate = primitive("concatenate", true);
    result.tail = primitive("tail", false);
    return result;
}

} // namespace markov::trace_graph::cost
