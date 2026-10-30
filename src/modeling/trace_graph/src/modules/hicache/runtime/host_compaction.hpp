#pragma once

#include <cstddef>
#include <span>

namespace markov::trace_graph::modules::hicache::runtime {

struct HiCacheHostExpansion;

// Collapse only private CPU chains; source identities and public ports survive.
void compact_host_program(HiCacheHostExpansion & plan, std::span<const size_t> source_callbacks);

} // namespace markov::trace_graph::modules::hicache::runtime
