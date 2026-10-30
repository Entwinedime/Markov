#pragma once

#include "markov/trace_graph/adapters/frameworks/sglang/allocator_preparation.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/source_calls.hpp"
#include "markov/trace_graph/simulation/cpu_task_queues.hpp"
#include "modules/hicache/model/source_input.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

/** Borrowed immutable source graph; the owning CLI keeps it alive for all targets. */
struct HiCacheSourcePreparation {
    HiCacheSourcePreparation(const core::DagGraph & graph, uint64_t begin_us, uint64_t end_us);

    adapters::sglang::HiCacheSourceIndex index;
    simulation::detail::CpuTaskQueues queues;
    std::shared_ptr<const model::HiCacheSourceInput> input;
    adapters::sglang::PhaseCallAudit phase_calls;
    adapters::sglang::AllocatorPreparationObservation allocator;
    std::vector<adapters::sglang::AllocatorRegion> allocator_regions;
};

} // namespace markov::trace_graph::modules::hicache::runtime
