#pragma once

#include "markov/trace_graph/adapters/frameworks/sglang/allocator_program.hpp"
#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/modules/hicache/model/phase_work.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::adapters::sglang {
namespace model = modules::hicache::model;

struct AllocatorPreparationSourceCall {
    size_t source_fact_id = 0;
    std::string pid, tid;
    uint64_t begin_us = 0, end_us = 0;
    std::optional<AllocatorSpecialization> specialization;
    bool required = false, first_load = false;
    std::string path;
    std::vector<std::pair<uint64_t, uint64_t>> intervals;
    uint64_t service_duration_us = 0; // Same-source CPU correction over the interval union.
    std::optional<size_t> submit_gap_node;
    std::map<std::string, uint64_t> issues;
    // Exact inner-call metadata for whole allocator replacement. These are
    // source observations, independent of preparation timing measurements.
    std::optional<size_t> allocator_observation;
    std::optional<model::HiCacheAllocatorWorkItem> allocator_work;
    std::optional<std::string> prefix_dtype;
    std::optional<bool> page_count_supplied;
    std::vector<model::HiCacheAllocatorRequest> requests;
};

/** Immutable base observations, reusable while the target DAG is executing. */
struct AllocatorPreparationObservation {
    std::vector<AllocatorPreparationSourceCall> calls;
    std::set<std::string> supported_pids;
};


/** Raw source work and corrected source service, never target cost inference. */
[[nodiscard]] AllocatorPreparationObservation observe_allocator_preparations(const core::DagGraph & graph);

/** The installed allocator ABI is shared by observation and static diagnostics. */
[[nodiscard]] bool is_allocator_preparation_event(const core::TraceEvent & event);
[[nodiscard]] std::optional<AllocatorSpecialization> allocator_preparation_specialization(const core::TraceEvent & event);
[[nodiscard]] std::vector<std::pair<uint64_t, uint64_t>> merge_allocator_preparation_intervals(std::vector<std::pair<uint64_t, uint64_t>> intervals);

} // namespace markov::trace_graph::adapters::sglang
