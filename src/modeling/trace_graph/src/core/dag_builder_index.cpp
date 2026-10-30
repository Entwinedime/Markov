/**
 * @file
 * @brief Creates base DAG nodes and temporary identity indices.
 */
#include "markov/trace_graph/core/dag_builder_stages.hpp"
#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace markov::trace_graph::core {

namespace {

using dag_builder_detail::is_device_sync_event;
using dag_builder_detail::is_event_sync_event;
using dag_builder_detail::is_stream_sync_event;
using dag_builder_detail::is_usable_lane_value;
using dag_builder_detail::resolve_event_lane;

bool has_connection_identity(std::string_view value) {
    // CANN's missing connection is -1, also exported as unsigned uint64.
    // Keep that raw observation, but never join unrelated events by it.
    static const auto unsigned_missing_connection = std::to_string(std::numeric_limits<uint64_t>::max());
    return !value.empty() && value != "-1" && value != unsigned_missing_connection;
}

class NodeIndexer {
public:
    explicit NodeIndexer(DagGraph & graph) : graph_(graph) {
        index_.lane_to_nodes.reserve(256);
        index_.stream_alias_to_lane.reserve(512);
        index_.device_sync_nodes.reserve(16);
        index_.notify_record_nodes.reserve(64);
        index_.notify_wait_nodes.reserve(64);
        index_.model_execute_nodes.reserve(64);
    }

    [[nodiscard]] DagBuildIndex run() {
        for (const auto event_index : std::views::iota(size_t{ 0 }, graph_.event_count())) add_event(event_index);
        return std::move(index_);
    }

private:
    void add_event(size_t event_index) {
        auto & event = graph_.mutable_event(event_index);
        auto identity = resolve_event_lane(event, true);
        const auto node_id = graph_.add_node(event_index, !identity.is_device, identity.lane);
        const auto lane_id = graph_.node(node_id).lane_id;
        index_.lane_to_nodes[lane_id].push_back(node_id);

        if (identity.is_device) register_device_aliases(event, identity, lane_id);
        index_causality_identity(event, node_id, "connection_id", index_.connection_to_nodes);
        if (event.has_arg_key_hint("enclosing_copy_connections"))
            for (const auto & connection : nlohmann::json::parse(event.arg("enclosing_copy_connections"))) {
                auto value = connection.get<std::string>();
                if (has_connection_identity(value)) index_.connection_to_nodes[std::move(value)].push_back(node_id);
            }
        index_causality_identity(event, node_id, "correlation_id", index_.correlation_to_nodes);
        classify_special_event(event, node_id);
    }

    void register_device_aliases(const TraceEvent & event, const dag_builder_detail::EventLaneIdentity & identity, size_t lane_id) {
        register_alias(event.tid, lane_id);
        if (identity.stream_id) register_alias(*identity.stream_id, lane_id);
        if (identity.alternate_stream_id) register_alias(*identity.alternate_stream_id, lane_id);
        if (identity.physical_stream_id) register_alias(*identity.physical_stream_id, lane_id);
    }

    void register_alias(const std::string & value, size_t lane_id) {
        if (is_usable_lane_value(value)) index_.stream_alias_to_lane[value] = lane_id;
    }

    static void index_causality_identity(const TraceEvent & event, size_t node_id, std::string_view key,
                                         std::unordered_map<std::string, std::vector<size_t>> & groups) {
        if (!event.has_arg_key_hint(key)) return;
        auto value = event.find_arg(key);
        if (!value || value->empty()) return;
        if (key == "connection_id" && !has_connection_identity(*value)) return;

        groups[std::move(*value)].push_back(node_id);
    }

    /** @brief Classifies synchronization anchors without changing observed time. */
    void classify_special_event(TraceEvent & event, size_t node_id) {
        if (event.boundary == TraceBoundary::DeviceEventRecord) index_.event_record_nodes.push_back(node_id);
        else if (event.boundary == TraceBoundary::DeviceEventWait) index_.event_wait_nodes.push_back(node_id);
        else if (event.boundary == TraceBoundary::StreamWait) index_.native_stream_wait_nodes.push_back(node_id);
        else if (is_stream_sync_event(event)) index_.stream_sync_nodes.push_back(node_id);
        else if (is_event_sync_event(event)) index_.event_sync_nodes.push_back(node_id);
        else if (is_device_sync_event(event)) index_.device_sync_nodes.push_back(node_id);
        else if (event.boundary == TraceBoundary::NotifyRecord) index_.notify_record_nodes.push_back(node_id);
        else if (event.boundary == TraceBoundary::NotifyWait) index_.notify_wait_candidate_count++;
        else if (event.boundary == TraceBoundary::ModelExecute) index_.model_execute_nodes.push_back(node_id);
    }

    DagGraph & graph_;
    DagBuildIndex index_;
};

} // namespace

DagBuildIndex create_node_index(DagGraph & graph) { return NodeIndexer(graph).run(); }

} // namespace markov::trace_graph::core
