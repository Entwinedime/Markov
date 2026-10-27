#pragma once

#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/structure/hicache/resources.hpp"

namespace markov::trace_graph::adapters::sglang {

/** Separate scheduler-preparation and forward stream identities. Allocator
 * submissions identify schedule work; layer checks identify forward work.
 * Observe before replacing source calls. Values are source graph node handles,
 * not costs or target work templates. Missing/ambiguous forward placements are
 * not fabricated; conflicting proven identities on one scheduler fail clearly.
 */
[[nodiscard]] structure::hicache::SchedulerDeviceResources observe_scheduler_compute_resources(const adapters::sglang::HiCacheSourceIndex & source);

} // namespace markov::trace_graph::adapters::sglang
