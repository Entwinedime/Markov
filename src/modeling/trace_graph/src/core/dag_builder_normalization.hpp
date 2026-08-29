/**
 * @file
 * @brief Internal event normalization and lane-identity contract for DAG construction.
 */
#pragma once

#include "markov/trace_graph/core/trace_event.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace markov::trace_graph::core {

namespace dag_builder_detail {

struct EventLaneIdentity {
    bool is_device = false;
    std::string lane = "CPU_MERGED";
    std::optional<std::string> stream_id;
    std::optional<std::string> alternate_stream_id;
    std::optional<std::string> physical_stream_id;
};

[[nodiscard]] uint64_t node_end_ts(const TraceEvent & event);
[[nodiscard]] bool is_hicache_control_event(const TraceEvent & event);
[[nodiscard]] EventLaneIdentity resolve_event_lane(const TraceEvent & event, bool collect_aliases);

} // namespace dag_builder_detail

/** @brief Deduplicates executable events and removes nested CPU parents. */
[[nodiscard]] std::vector<TraceEvent> normalize_events(std::vector<TraceEvent> events);

} // namespace markov::trace_graph::core
