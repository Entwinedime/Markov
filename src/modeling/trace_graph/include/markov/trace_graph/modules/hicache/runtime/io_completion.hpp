#pragma once

#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/structure/hicache/io_completion.hpp"

namespace markov::trace_graph::modules::hicache::runtime {

/** Bind immutable numerical models to the execution clock. The callbacks own
 * their model input and can be shared by all I/O consumers of one target. */
[[nodiscard]] structure::hicache::IoCompletionProvider io_completion_provider(const frontend::HiCacheIoCostConfig & config);

} // namespace markov::trace_graph::modules::hicache::runtime
