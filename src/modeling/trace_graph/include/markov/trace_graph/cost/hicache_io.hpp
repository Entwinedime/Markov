#pragma once

#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/structure/hicache/io_completion.hpp"

namespace markov::trace_graph::cost {

/** Owned callbacks keep numerical data outside the cache policy/state object. */
[[nodiscard]] structure::hicache::IoCompletionProvider io_completion_provider(const frontend::HiCacheIoCostConfig & config);

} // namespace markov::trace_graph::cost
