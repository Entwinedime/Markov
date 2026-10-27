#include "markov/trace_graph/modules/hicache/runtime/io_completion.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/cost/io_service.hpp"

#include <memory>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

structure::hicache::IoCompletionProvider io_completion_provider(const frontend::HiCacheIoCostConfig & config) {
    const auto models = std::make_shared<const frontend::HiCacheIoCostConfig>(config);
    return {
        .duration =
            [models](const std::string & kind, uint64_t bytes, uint64_t pages, uint64_t existing) {
                const auto & service = cost::require_io_service(*models, kind, bytes, pages, existing);
                const auto estimate = cost::estimate_io_service(service, bytes, pages, 1, existing);
                if (!estimate) throw std::runtime_error("I/O work is outside the applicable service formula");

                return estimate->duration_us;
            },
        .prefetch_batch =
            [models](uint64_t bytes, uint64_t pages, uint64_t start_us) {
                const auto & service = cost::require_io_service(*models, "prefetch", bytes, pages);
                const auto timing = cost::estimate_prefetch_service(service, bytes, pages);
                if (!timing) throw std::runtime_error("Prefetch requires applicable read/copy/publication timings");

                PrefetchControlBatch batch{
                    .page_count = pages,
                    .start_ts = start_us,
                    .ready_ts = core::checked_add_u64(start_us, timing->duration_us, "prefetch service time overflow"),
                };
                for (const auto & page : timing->publications) {
                    batch.publications.push_back({
                        1,
                        core::checked_add_u64(start_us, page.publication_offset_us, "prefetch service time overflow"),
                        core::checked_add_u64(start_us, page.cancelled_duration_us, "prefetch service time overflow"),
                    });
                }
                return batch;
            },
    };
}

} // namespace markov::trace_graph::modules::hicache::runtime
