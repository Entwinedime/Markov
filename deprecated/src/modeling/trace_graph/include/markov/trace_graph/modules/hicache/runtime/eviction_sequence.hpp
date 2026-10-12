#pragma once
#include "markov/trace_graph/modules/hicache/model/state.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_host.hpp"
#include <string>

namespace markov::trace_graph::modules::hicache::runtime {

/** Logical target work only. Providers supply costs later; a victim index
 * refers to target state, never to a donor's object or completion event. */
struct HiCacheEvictionStep {
    HiCacheEvictionRegion::Kind kind;
    HiCacheEvictionRegion::ControlPhase control_phase = HiCacheEvictionRegion::ControlPhase::None;
    std::optional<size_t> victim;
    std::optional<HiCacheEvictionRegion::Kind> preceding_kind;
    size_t heap_size = 0;
};

[[nodiscard]] std::vector<HiCacheEvictionStep> eviction_sequence(const model::HiCacheEvictionWork & work, bool write_back);

/** Semantic primitive identity shared by observation export and execution. */
[[nodiscard]] std::string eviction_control_name(HiCacheEvictionRegion::ControlPhase phase);

} // namespace markov::trace_graph::modules::hicache::runtime
