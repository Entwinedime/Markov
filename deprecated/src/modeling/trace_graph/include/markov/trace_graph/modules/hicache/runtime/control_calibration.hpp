#pragma once
// Independent CPU/control observation export; not target program import.
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include <cstdint>
#include <map>
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace markov::trace_graph::modules::hicache::runtime {

/** Observe portable write and eviction programs; CPU corrections must already
 * be attached to the source graph. Release-only extraction requires ordinary
 * release observations, not a write operation. Does not consume target data. */
[[nodiscard]] nlohmann::json export_write_calibration(core::DagGraph & graph, uint64_t begin, uint64_t end, bool release_only = false);

[[nodiscard]] nlohmann::json export_host_template(const core::DagGraph & graph, const HiCacheHostExpansion & plan, const std::map<std::string, size_t> & roles);
[[nodiscard]] nlohmann::json export_write_template(const core::DagGraph & graph, const HiCacheWriteExpansion & plan,
                                                   const std::map<std::string, size_t> & roles);
} // namespace markov::trace_graph::modules::hicache::runtime
