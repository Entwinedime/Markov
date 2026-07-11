/**
 * @file
 * @brief Cross-channel trace normalization implementation.
 */
#include "markov/trace_graph/frontend/trace_normalizer.hpp"

namespace markov::trace_graph::frontend {

void normalize_trace_events(std::vector<core::TraceEvent> & events) {
    for (auto & event : events) {
        if (event.cat == "hicache" || event.name.starts_with("HiCache::") || event.name.starts_with("hicache_")) {
            event.cat = "hicache";
            if (event.arg("domain") != "hicache") event.set_arg("domain", "hicache");
        }
    }
}

} // namespace markov::trace_graph::frontend
