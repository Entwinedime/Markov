#include "markov/trace_graph/cost/hicache_io.hpp"

#include "markov/trace_graph/cost/missing_evidence.hpp"
#include "markov/trace_graph/modules/hicache/service_model.hpp"
#include <memory>
#include <stdexcept>

namespace markov::trace_graph::cost {

structure::hicache::IoCompletionProvider io_completion_provider(const frontend::HiCacheIoCostConfig & config) {
    const auto models = std::make_shared<const frontend::HiCacheIoCostConfig>(config);
    return {
        .duration =
            [models](const std::string & kind, uint64_t bytes, uint64_t pages, uint64_t existing) {
                const auto & service = modules::hicache::require_hicache_service(*models, kind, bytes, pages, existing);
                const auto cost = modules::hicache::hicache_service_cost(service, bytes, pages, 1, existing);
                if (!cost) throw std::runtime_error("I/O work is outside the applicable service formula");

                return cost->duration_us;
            },
        .prefetch_batch =
            [models](uint64_t bytes, uint64_t pages, uint64_t start_us) {
                const auto & service = modules::hicache::require_hicache_service(*models, "prefetch", bytes, pages);
                const auto batch = modules::hicache::hicache_prefetch_batch(service, bytes, pages, start_us);
                if (!batch) throw std::runtime_error("Prefetch requires applicable read/copy/publication timings");

                return *batch;
            },
    };
}

} // namespace markov::trace_graph::cost
