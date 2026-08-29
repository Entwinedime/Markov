/**
 * @file
 * @brief Internal indices and ordered stages for base DAG construction.
 */
#pragma once

#include "dag_builder_normalization.hpp"

#include "markov/trace_graph/core/dag_graph.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::core {

namespace dag_builder_detail {

[[nodiscard]] bool is_usable_lane_value(const std::string & value);
[[nodiscard]] bool is_stream_sync_event(const std::string & name);
[[nodiscard]] bool is_event_sync_event(const std::string & name);
[[nodiscard]] bool is_device_sync_event(const std::string & name);

} // namespace dag_builder_detail

struct DagBuildIndex {
    std::unordered_map<size_t, std::vector<size_t>> lane_to_nodes;
    std::unordered_map<std::string, std::vector<size_t>> correlation_to_nodes;
    std::unordered_map<std::string, std::vector<size_t>> connection_to_nodes;
    std::unordered_map<std::string, std::vector<size_t>> event_id_to_nodes;
    std::unordered_map<std::string, size_t> raw_stream_to_lane;
    std::unordered_map<std::string, size_t> stream_alias_to_lane;
    std::vector<size_t> event_record_nodes;
    std::vector<size_t> event_wait_nodes;
    std::vector<size_t> stream_sync_nodes;
    std::vector<size_t> event_sync_nodes;
    std::vector<size_t> device_sync_nodes;
    std::vector<size_t> notify_record_nodes;
    std::vector<size_t> notify_wait_nodes;
    size_t notify_wait_candidate_count = 0;
    std::vector<size_t> model_execute_nodes;
};

[[nodiscard]] DagBuildIndex create_node_index(DagGraph & graph);
void sort_nodes_by_event_ts_if_needed(const DagGraph & graph, std::vector<size_t> & nodes);
[[nodiscard]] size_t estimate_edge_capacity(const DagGraph & graph, const DagBuildIndex & index);
void add_correlation_edges(DagGraph & graph, DagBuildIndex & index, size_t threads);
void add_sequential_edges(DagGraph & graph, DagBuildIndex & index);
void add_request_boundary_edges(DagGraph & graph, DagBuildIndex & index);
void add_event_wait_edges(DagGraph & graph, DagBuildIndex & index);
void add_notify_wait_edges(DagGraph & graph, DagBuildIndex & index);
void add_model_execute_edges(DagGraph & graph, DagBuildIndex & index);
void add_stream_sync_edges(DagGraph & graph, DagBuildIndex & index);
void add_event_sync_edges(DagGraph & graph, DagBuildIndex & index);
void add_device_sync_edges(DagGraph & graph, DagBuildIndex & index);
void finalize_sync_nodes(DagGraph & graph, const DagBuildIndex & index);

} // namespace markov::trace_graph::core
