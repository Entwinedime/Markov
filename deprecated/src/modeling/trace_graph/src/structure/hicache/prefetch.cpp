#include "markov/trace_graph/structure/hicache/prefetch.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <stdexcept>

namespace markov::trace_graph::structure::hicache {

modules::hicache::PrefetchBatchExecution execute_prefetch_batch(const modules::hicache::PrefetchControlBatch & batch, std::optional<uint64_t> stop_us) {
    modules::hicache::PrefetchBatchExecution result{ .release_us = batch.ready_ts };
    for (const auto & publication : batch.publications) {
        result.copied_pages = core::checked_add_u64(result.copied_pages, publication.page_count, "prefetch copied pages overflow");
        if (stop_us && publication.at_us >= *stop_us) {
            if (!publication.cancelled_return_us) throw std::invalid_argument("cancelled prefetch requires a measured return-path timing");

            result.release_us = *publication.cancelled_return_us;
            result.cancelled = true;
            break;
        }

        result.published_pages = result.copied_pages;
    }
    return result;
}

} // namespace markov::trace_graph::structure::hicache
