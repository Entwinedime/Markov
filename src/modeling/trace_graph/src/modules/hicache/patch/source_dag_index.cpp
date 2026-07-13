/**
 * @file
 * @brief Compact source-DAG index implementation.
 */
#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"

#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/fact.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <string_view>

namespace markov::trace_graph::modules::hicache::patch {

namespace source_dag_index_detail {

using Json = nlohmann::json;

template <typename Index> void append_identity(const core::TraceEvent & event, std::string_view key, size_t node_id, Index & index) {
    if (!event.has_arg_key_hint(key)) return;
    auto value = event.arg(key);
    if (!value.empty()) index[std::move(value)].push_back(node_id);
}

bool fact_candidate(const core::TraceEvent & event) { return event.source_channel == core::TraceSourceChannel::PythonProbe && event.has_arg_key_hint("fact"); }

Json array_arg(const core::TraceEvent & event, std::string_view key) {
    if (!event.has_arg_key_hint(key)) return Json::array();
    const auto value = event.arg(key);
    if (value.empty()) return Json::array();
    try {
        auto parsed = Json::parse(value);
        return parsed.is_array() ? std::move(parsed) : Json::array();
    }
    catch (const Json::exception &) {
        return Json::array();
    }
}

std::vector<uint64_t> u64_array_arg(const core::TraceEvent & event, std::string_view key) {
    std::vector<uint64_t> values;
    for (const auto & item : array_arg(event, key)) {
        if (item.is_number_unsigned()) values.push_back(item.get<uint64_t>());
        else if (item.is_number_integer()) {
            const auto value = item.get<int64_t>();
            if (value >= 0) values.push_back(static_cast<uint64_t>(value));
        }
    }
    return values;
}

std::vector<std::string> string_array_arg(const core::TraceEvent & event, std::string_view key) {
    std::vector<std::string> values;
    for (const auto & item : array_arg(event, key)) {
        if (item.is_string()) values.push_back(item.get<std::string>());
    }
    return values;
}

std::optional<bool> bool_arg(const core::TraceEvent & event, std::string_view key) {
    if (!event.has_arg_key_hint(key)) return std::nullopt;
    const auto value = event.arg(key);
    if (value == "true" || value == "True" || value == "1") return true;
    if (value == "false" || value == "False" || value == "0") return false;
    return std::nullopt;
}

} // namespace source_dag_index_detail

HiCacheSourceDagIndex::HiCacheSourceDagIndex(const core::DagGraph & graph) : graph_(graph) {
    stats_.stored_node_count = graph.node_count();
    stats_.dag_patch_contract_ready = graph.has_input_contract("hicache_dag_patch");
    incoming_offsets_.assign(graph.node_count() + 1, 0);
    outgoing_offsets_.assign(graph.node_count() + 1, 0);

    for (const auto & node : graph.nodes()) {
        if (!node.active) continue;
        ++stats_.active_node_count;
        const auto & event = graph.event_for_node(node.id);
        if (event.source_channel != core::TraceSourceChannel::PythonProbe) continue;
        source_dag_index_detail::append_identity(event, "request_id", node.id, nodes_by_request_);
        source_dag_index_detail::append_identity(event, "operation_id", node.id, nodes_by_operation_);

        if (!source_dag_index_detail::fact_candidate(event)) continue;
        try {
            auto metadata = parse_hicache_fact_metadata(event);
            auto operation_id = event.arg("operation_id");
            if (operation_id.empty() && event.has_arg_key_hint("node_id")) operation_id = event.arg("node_id");
            std::optional<uint64_t> object_node_id;
            if (event.has_arg_key_hint("node_id")) {
                const auto raw_node_id = event.find_arg("node_id");
                if (raw_node_id) object_node_id = core::parse_u64(*raw_node_id);
            }
            const auto fact_index = fact_nodes_.size();
            fact_nodes_.push_back(HiCacheSourceFactNode{
                .node_id = node.id,
                .event_index = event.index,
                .timestamp_us = event.ts,
                .duration_us = node.duration,
                .pid = event.pid,
                .tid = event.tid,
                .target_id = event.arg("target_id"),
                .phase = event.arg("phase"),
                .fact_class = std::move(metadata.fact_class),
                .fact_role = std::move(metadata.role),
                .request_id = event.arg("request_id"),
                .batch_request_ids = source_dag_index_detail::string_array_arg(event, "batch_request_ids"),
                .operation_id = std::move(operation_id),
                .cache_scope = event.arg("cache_scope"),
                .object_node_id = object_node_id,
                .full_path_span = parse_hicache_token_span_arg(event, "full_path_span"),
                .token_count = event.arg_u64("token_count", 0),
                .effective_token_count = event.arg_u64("effective_token_count", 0),
                .completed_token_count = event.arg_u64("completed_token_count", 0),
                .progress_ready = source_dag_index_detail::bool_arg(event, "progress_ready"),
                .operation_node_ids = source_dag_index_detail::u64_array_arg(event, "operation_node_ids"),
                .page_hashes = source_dag_index_detail::string_array_arg(event, "page_hashes"),
            });
            fact_index_by_node_.emplace(node.id, fact_index);
            nodes_by_fact_role_[fact_nodes_.back().fact_role].push_back(node.id);
            for (const auto & request_id : fact_nodes_.back().batch_request_ids) nodes_by_request_[request_id].push_back(node.id);
            ++stats_.counts_by_fact_class[fact_nodes_.back().fact_class];
            ++stats_.counts_by_fact_role[fact_nodes_.back().fact_role];
            if (!fact_nodes_.back().request_id.empty() || !fact_nodes_.back().batch_request_ids.empty())
                ++stats_.request_identity_counts_by_fact_role[fact_nodes_.back().fact_role];
            if (!fact_nodes_.back().operation_id.empty()) ++stats_.operation_identity_counts_by_fact_role[fact_nodes_.back().fact_role];
            if (fact_nodes_.back().fact_class == "workload_identity") ++stats_.workload_identity_fact_count;
            else if (fact_nodes_.back().fact_class == "source_actual") ++stats_.source_actual_fact_count;
            else if (fact_nodes_.back().fact_class == "timing_observation") ++stats_.timing_observation_fact_count;
        }
        catch (const std::exception &) {
            ++stats_.malformed_fact_count;
        }
    }

    for (const auto & edge : graph.edges()) {
        if (!edge.active) continue;
        if (edge.src >= graph.node_count() || edge.dst >= graph.node_count()) throw std::logic_error("Source DAG index found an edge with an invalid endpoint");
        if (!graph.node(edge.src).active || !graph.node(edge.dst).active)
            throw std::logic_error("Source DAG index found an active edge attached to an inactive node");
        ++outgoing_offsets_[edge.src + 1];
        ++incoming_offsets_[edge.dst + 1];
        ++stats_.active_edge_count;
    }
    for (size_t node_id = 0; node_id < graph.node_count(); ++node_id) {
        outgoing_offsets_[node_id + 1] += outgoing_offsets_[node_id];
        incoming_offsets_[node_id + 1] += incoming_offsets_[node_id];
    }
    outgoing_edge_ids_.resize(stats_.active_edge_count);
    incoming_edge_ids_.resize(stats_.active_edge_count);
    auto outgoing_cursor = outgoing_offsets_;
    auto incoming_cursor = incoming_offsets_;
    for (size_t edge_index = 0; edge_index < graph.edge_count(); ++edge_index) {
        const auto & edge = graph.edge(edge_index);
        if (!edge.active) continue;
        outgoing_edge_ids_[outgoing_cursor[edge.src]++] = edge_index;
        incoming_edge_ids_[incoming_cursor[edge.dst]++] = edge_index;
    }

    fact_nodes_in_time_order_.reserve(fact_nodes_.size());
    std::ranges::transform(fact_nodes_, std::back_inserter(fact_nodes_in_time_order_), [](const auto & fact) { return fact.node_id; });
    std::ranges::sort(fact_nodes_in_time_order_, [&](size_t left_node_id, size_t right_node_id) {
        const auto * left = fact_node(left_node_id);
        const auto * right = fact_node(right_node_id);
        if (left->timestamp_us != right->timestamp_us) return left->timestamp_us < right->timestamp_us;
        if (left->pid != right->pid) return left->pid < right->pid;
        if (left->tid != right->tid) return left->tid < right->tid;
        const auto & left_event = graph.event_for_node(left_node_id);
        const auto & right_event = graph.event_for_node(right_node_id);
        if (left_event.name != right_event.name) return left_event.name < right_event.name;
        if (left->event_index != right->event_index) return left->event_index < right->event_index;
        return left_node_id < right_node_id;
    });

    stats_.fact_node_count = fact_nodes_.size();
    stats_.request_identity_count = nodes_by_request_.size();
    stats_.operation_identity_count = nodes_by_operation_.size();
    stats_.status = stats_.malformed_fact_count == 0 ? "ready" : "partial";
}

std::span<const size_t> HiCacheSourceDagIndex::incoming_edge_ids(size_t node_id) const {
    if (node_id >= graph_.node_count()) throw std::out_of_range("Source DAG incoming adjacency node ID is out of range");
    return std::span<const size_t>{ incoming_edge_ids_ }.subspan(incoming_offsets_[node_id], incoming_offsets_[node_id + 1] - incoming_offsets_[node_id]);
}

std::span<const size_t> HiCacheSourceDagIndex::outgoing_edge_ids(size_t node_id) const {
    if (node_id >= graph_.node_count()) throw std::out_of_range("Source DAG outgoing adjacency node ID is out of range");
    return std::span<const size_t>{ outgoing_edge_ids_ }.subspan(outgoing_offsets_[node_id], outgoing_offsets_[node_id + 1] - outgoing_offsets_[node_id]);
}

const HiCacheSourceFactNode * HiCacheSourceDagIndex::fact_node(size_t node_id) const {
    const auto it = fact_index_by_node_.find(node_id);
    return it == fact_index_by_node_.end() ? nullptr : &fact_nodes_[it->second];
}

std::span<const size_t> HiCacheSourceDagIndex::find_nodes(const NodeMap & index, std::string_view key) {
    const auto it = index.find(key);
    return it == index.end() ? std::span<const size_t>{} : std::span<const size_t>{ it->second };
}

std::span<const size_t> HiCacheSourceDagIndex::nodes_for_fact_role(std::string_view role) const { return find_nodes(nodes_by_fact_role_, role); }

std::span<const size_t> HiCacheSourceDagIndex::nodes_for_request(std::string_view request_id) const { return find_nodes(nodes_by_request_, request_id); }

std::span<const size_t> HiCacheSourceDagIndex::nodes_for_operation(std::string_view operation_id) const {
    return find_nodes(nodes_by_operation_, operation_id);
}

} // namespace markov::trace_graph::modules::hicache::patch
