/**
 * @file
 * @brief Compact source-DAG index implementation.
 */
#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"

#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/fact.hpp"
#include "markov/trace_graph/modules/hicache/runtime/target_pager.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iterator>
#include <limits>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace markov::trace_graph::modules::hicache::patch {

namespace source_dag_index_detail {

using Json = nlohmann::json;

uint64_t saturated_add(uint64_t start, uint64_t duration) {
    return duration > std::numeric_limits<uint64_t>::max() - start ? std::numeric_limits<uint64_t>::max() : start + duration;
}

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

std::vector<std::string> request_ids_arg(const core::TraceEvent & event) {
    auto values = string_array_arg(event, "request_ids");
    return values.empty() ? string_array_arg(event, "batch_request_ids") : values;
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

    struct PendingControlInterval {
        std::string event_name;
        ControlInterval interval;
    };
    std::unordered_map<std::string, PendingControlInterval> pending_control_intervals;

    for (const auto & node : graph.nodes()) {
        if (!node.active) continue;
        ++stats_.active_node_count;
        if (node.is_cpu) {
            const auto & event = graph.event_for_node(node.id);
            auto lane_key = cpu_lane_key(event.pid, event.tid);
            auto [lane, inserted] = cpu_nodes_by_lane_.try_emplace(lane_key);
            (void)inserted;
            cpu_lane_keys_by_logical_input_[node.gpu_id].push_back(lane_key);
            lane->second.push_back(node.id);
            if (event.name.starts_with("hicache.control.") && event.name.ends_with(".self") && event.has_arg_key_hint("hicache_control_parent")
                && event.has_arg_key_hint("hicache_control_parent_index")) {
                const auto parent = event.arg("hicache_control_parent");
                const auto parent_index = event.arg("hicache_control_parent_index");
                if (!parent.empty() && !parent_index.empty()) {
                    const auto identity = parent + "\x1f" + event.pid + "\x1f" + event.tid + "\x1f" + parent_index;
                    const auto event_end = source_dag_index_detail::saturated_add(event.ts, event.dur);
                    auto [control, inserted] = pending_control_intervals.try_emplace(identity,
                                                                                     PendingControlInterval{
                                                                                         .event_name = parent,
                                                                                         .interval =
                                                                                             ControlInterval{
                                                                                                             .pid = event.pid,
                                                                                                             .tid = event.tid,
                                                                                                             .start_us = event.ts,
                                                                                                             .end_us = event_end,
                                                                                                             },
                    });
                    if (!inserted) {
                        control->second.interval.start_us = std::min(control->second.interval.start_us, event.ts);
                        control->second.interval.end_us = std::max(control->second.interval.end_us, event_end);
                    }
                }
            }
        }
    }
    for (auto & pending : pending_control_intervals | std::views::values) {
        control_intervals_by_name_[pending.event_name].push_back(std::move(pending.interval));
    }
    for (auto & intervals : control_intervals_by_name_ | std::views::values) {
        std::ranges::sort(intervals, [](const auto & left, const auto & right) {
            if (left.start_us != right.start_us) return left.start_us < right.start_us;
            if (left.end_us != right.end_us) return left.end_us < right.end_us;
            if (left.pid != right.pid) return left.pid < right.pid;
            return left.tid < right.tid;
        });
    }

    HiCacheFactParser source_fact_parser;
    for (const auto & event : graph.hicache_fact_events()) source_fact_parser.observe_token_dictionaries(event);
    for (const auto & event : graph.tail_context_events()) source_fact_parser.observe_token_dictionaries(event);
    const runtime::HiCacheTargetPager source_pager;

    const auto append_fact = [&](const core::TraceEvent & event, size_t fact_id, std::optional<size_t> execution_anchor_node_id) {
        if (!source_dag_index_detail::fact_candidate(event)) return;
        try {
            auto metadata = parse_hicache_fact_metadata(event);
            auto operation_id = event.arg("operation_id");
            if (operation_id.empty() && event.has_arg_key_hint("node_id")) operation_id = event.arg("node_id");
            std::optional<uint64_t> object_node_id;
            if (event.has_arg_key_hint("node_id")) {
                const auto raw_node_id = event.find_arg("node_id");
                if (raw_node_id) object_node_id = core::parse_u64(*raw_node_id);
            }
            std::vector<std::string> source_page_hashes;
            if (metadata.role == "cache_lifecycle_commit") {
                const auto parsed_fact = source_fact_parser.parse(fact_id, event, execution_anchor_node_id);
                const auto page_path = source_pager.project(parsed_fact, parsed_fact.full_path_tokens);
                source_page_hashes.reserve(page_path.pages.size());
                std::ranges::transform(page_path.pages, std::back_inserter(source_page_hashes), [](const auto & page) { return page.hash; });
            }
            const auto fact_index = fact_nodes_.size();
            fact_nodes_.push_back(HiCacheSourceFactNode{
                .node_id = fact_id,
                .execution_anchor_node_id = execution_anchor_node_id,
                .event_index = event.index,
                .event_name = event.name,
                .timestamp_us = event.ts,
                .duration_us = event.dur,
                .pid = event.pid,
                .tid = event.tid,
                .target_id = event.arg("target_id"),
                .phase = event.arg("phase"),
                .fact_class = std::move(metadata.fact_class),
                .fact_role = std::move(metadata.role),
                .request_id = event.arg("request_id"),
                .batch_request_ids = source_dag_index_detail::request_ids_arg(event),
                .operation_id = std::move(operation_id),
                .cache_scope = event.arg("cache_scope"),
                .object_node_id = object_node_id,
                .full_path_span = parse_hicache_token_span_arg(event, "full_path_span"),
                .source_page_size = event.arg_u64("source_page_size", 0),
                .service_item_count = event.arg_u64("service_item_count", 0),
                .token_count = event.arg_u64("token_count", 0),
                .effective_token_count = event.arg_u64("effective_token_count", 0),
                .completed_token_count = event.arg_u64("completed_token_count", 0),
                .completed_token_count_present = event.has_arg_key_hint("completed_token_count"),
                .progress_ready = source_dag_index_detail::bool_arg(event, "progress_ready"),
                .write_back = source_dag_index_detail::bool_arg(event, "write_back"),
                .operation_node_ids = source_dag_index_detail::u64_array_arg(event, "operation_node_ids"),
                .page_hashes = source_dag_index_detail::string_array_arg(event, "page_hashes"),
                .source_page_hashes = std::move(source_page_hashes),
            });
            fact_index_by_node_.emplace(fact_id, fact_index);
            source_dag_index_detail::append_identity(event, "request_id", fact_id, nodes_by_request_);
            source_dag_index_detail::append_identity(event, "operation_id", fact_id, nodes_by_operation_);
            nodes_by_fact_role_[fact_nodes_.back().fact_role].push_back(fact_id);
            for (const auto & request_id : fact_nodes_.back().batch_request_ids) nodes_by_request_[request_id].push_back(fact_id);
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
    };

    for (const auto & event : graph.hicache_fact_events()) append_fact(event, event.index, std::nullopt);

    for (const auto & event : graph.tail_context_events()) {
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
            const auto tail_fact_index = tail_context_facts_.size();
            if (tail_fact_index > std::numeric_limits<size_t>::max() - graph.node_count())
                throw std::overflow_error("HiCache tail fact identity exceeds size_t range");
            const auto tail_fact_node_id = graph.node_count() + tail_fact_index;
            tail_context_facts_.push_back(HiCacheSourceFactNode{
                .node_id = tail_fact_node_id,
                .event_index = event.index,
                .event_name = event.name,
                .timestamp_us = event.ts,
                .duration_us = event.dur,
                .pid = event.pid,
                .tid = event.tid,
                .target_id = event.arg("target_id"),
                .phase = event.arg("phase"),
                .fact_class = std::move(metadata.fact_class),
                .fact_role = std::move(metadata.role),
                .request_id = event.arg("request_id"),
                .batch_request_ids = source_dag_index_detail::request_ids_arg(event),
                .operation_id = std::move(operation_id),
                .cache_scope = event.arg("cache_scope"),
                .object_node_id = object_node_id,
                .full_path_span = parse_hicache_token_span_arg(event, "full_path_span"),
                .source_page_size = event.arg_u64("source_page_size", 0),
                .service_item_count = event.arg_u64("service_item_count", 0),
                .token_count = event.arg_u64("token_count", 0),
                .effective_token_count = event.arg_u64("effective_token_count", 0),
                .completed_token_count = event.arg_u64("completed_token_count", 0),
                .completed_token_count_present = event.has_arg_key_hint("completed_token_count"),
                .progress_ready = source_dag_index_detail::bool_arg(event, "progress_ready"),
                .write_back = source_dag_index_detail::bool_arg(event, "write_back"),
                .operation_node_ids = source_dag_index_detail::u64_array_arg(event, "operation_node_ids"),
                .page_hashes = source_dag_index_detail::string_array_arg(event, "page_hashes"),
            });
            tail_fact_index_by_node_.emplace(tail_fact_node_id, tail_fact_index);
        }
        catch (const std::exception &) {
            // Tail context is optional closure evidence; malformed rows cannot prove closure.
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
        if (left->event_name != right->event_name) return left->event_name < right->event_name;
        if (left->event_index != right->event_index) return left->event_index < right->event_index;
        return left_node_id < right_node_id;
    });
    for (auto & nodes : cpu_nodes_by_lane_ | std::views::values) {
        std::ranges::sort(nodes, [&](size_t left_node_id, size_t right_node_id) {
            const auto & left = graph_.event_for_node(left_node_id);
            const auto & right = graph_.event_for_node(right_node_id);
            if (left.ts != right.ts) return left.ts < right.ts;
            return left_node_id < right_node_id;
        });
    }
    for (auto & lane_keys : cpu_lane_keys_by_logical_input_ | std::views::values) {
        std::ranges::sort(lane_keys);
        lane_keys.erase(std::unique(lane_keys.begin(), lane_keys.end()), lane_keys.end());
    }

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
    if (it != fact_index_by_node_.end()) return &fact_nodes_[it->second];
    const auto tail = tail_fact_index_by_node_.find(node_id);
    return tail == tail_fact_index_by_node_.end() ? nullptr : &tail_context_facts_[tail->second];
}

std::span<const size_t> HiCacheSourceDagIndex::find_nodes(const NodeMap & index, std::string_view key) {
    const auto it = index.find(key);
    return it == index.end() ? std::span<const size_t>{} : std::span<const size_t>{ it->second };
}

std::span<const size_t> HiCacheSourceDagIndex::nodes_for_fact_role(std::string_view role) const { return find_nodes(nodes_by_fact_role_, role); }

std::span<const size_t> HiCacheSourceDagIndex::nodes_for_request(std::string_view request_id) const { return find_nodes(nodes_by_request_, request_id); }

std::span<const size_t> HiCacheSourceDagIndex::nodes_for_operation(std::string_view operation_id) const { return find_nodes(nodes_by_operation_, operation_id); }

std::string HiCacheSourceDagIndex::cpu_lane_key(std::string_view pid, std::string_view tid) { return std::string(pid) + "\x1f" + std::string(tid); }

std::optional<size_t> HiCacheSourceDagIndex::cpu_boundary_at_or_before(std::string_view pid, std::string_view tid, uint64_t timestamp_us) const {
    const auto nodes = find_nodes(cpu_nodes_by_lane_, cpu_lane_key(pid, tid));
    const auto bound = std::upper_bound(nodes.begin(), nodes.end(), timestamp_us, [&](uint64_t timestamp, size_t node_id) {
        return timestamp < graph_.event_for_node(node_id).ts;
    });
    if (bound == nodes.begin()) return std::nullopt;
    return *(bound - 1);
}

std::optional<size_t> HiCacheSourceDagIndex::cpu_boundary_at_or_after(std::string_view pid, std::string_view tid, uint64_t timestamp_us) const {
    const auto nodes = find_nodes(cpu_nodes_by_lane_, cpu_lane_key(pid, tid));
    const auto bound = std::lower_bound(nodes.begin(), nodes.end(), timestamp_us, [&](size_t node_id, uint64_t timestamp) {
        return graph_.event_for_node(node_id).ts < timestamp;
    });
    if (bound != nodes.begin()) {
        const auto previous = *(bound - 1);
        const auto & event = graph_.event_for_node(previous);
        const auto event_end = source_dag_index_detail::saturated_add(event.ts, event.dur);
        if (timestamp_us <= event_end) return previous;
    }
    return bound == nodes.end() ? std::nullopt : std::optional<size_t>{ *bound };
}

HiCacheTimingIntervalOwnership HiCacheSourceDagIndex::timing_interval_ownership(const HiCacheSourceFactNode & fact) const { return timing_interval_ownership(fact.pid, fact.tid, fact.timestamp_us, fact.duration_us); }

HiCacheTimingIntervalOwnership HiCacheSourceDagIndex::timing_interval_ownership(std::string_view pid, std::string_view tid, uint64_t start_us,
                                                                                uint64_t duration_us) const {
    HiCacheTimingIntervalOwnership output{
        .interval_start_us = start_us,
        .interval_end_us = source_dag_index_detail::saturated_add(start_us, duration_us),
        .observed_duration_us = duration_us,
    };
    if (duration_us == 0) {
        output.status = "zero_duration";
        output.reason = "timing observation has zero duration";
        return output;
    }
    const auto nodes = find_nodes(cpu_nodes_by_lane_, cpu_lane_key(pid, tid));
    if (nodes.empty()) {
        output.reason = "timing observation has no executable nodes on the same pid/tid lane";
        return output;
    }
    output.start_anchor_node_id = cpu_boundary_at_or_before(pid, tid, output.interval_start_us);
    output.completion_anchor_node_id = cpu_boundary_at_or_after(pid, tid, output.interval_end_us);

    bool partial_node_overlap = false;
    for (size_t node_id : nodes) {
        const auto & event = graph_.event_for_node(node_id);
        const auto event_end = source_dag_index_detail::saturated_add(event.ts, event.dur);
        const auto overlap_start = std::max(output.interval_start_us, event.ts);
        const auto overlap_end = std::min(output.interval_end_us, event_end);
        if (overlap_end <= overlap_start) continue;
        if (event.ts < output.interval_start_us || event_end > output.interval_end_us) {
            partial_node_overlap = true;
            continue;
        }
        output.owned_node_ids.push_back(node_id);
        output.owned_node_duration_us =
            core::checked_add_u64(output.owned_node_duration_us, event.dur, "HiCache timing-owned node duration exceeds uint64 range");
    }

    if (!output.owned_node_ids.empty()) {
        auto frontier_node_id = output.owned_node_ids.front();
        const auto & first = graph_.event_for_node(frontier_node_id);
        auto frontier_end = source_dag_index_detail::saturated_add(first.ts, first.dur);
        for (size_t index = 1; index < output.owned_node_ids.size(); ++index) {
            const auto overlapping_node_id = output.owned_node_ids[index];
            const auto & overlapping = graph_.event_for_node(overlapping_node_id);
            const auto overlapping_end = source_dag_index_detail::saturated_add(overlapping.ts, overlapping.dur);
            const auto overlap_end = std::min(frontier_end, overlapping_end);
            if (overlap_end > overlapping.ts) {
                const auto duration = overlap_end - overlapping.ts;
                const auto & frontier = graph_.event_for_node(frontier_node_id);
                output.overlapping_node_slices.push_back(HiCacheCpuOverlapSlice{
                    .frontier_node_id = frontier_node_id,
                    .overlapping_node_id = overlapping_node_id,
                    .frontier_event_name = frontier.name,
                    .overlapping_event_name = overlapping.name,
                    .overlap_start_us = overlapping.ts,
                    .overlap_end_us = overlap_end,
                });
                output.overlapping_node_duration_us =
                    core::checked_add_u64(output.overlapping_node_duration_us, duration, "HiCache overlapping CPU leaf duration exceeds uint64 range");
                output.max_node_overlap_us = std::max(output.max_node_overlap_us, duration);
            }
            if (overlapping_end > frontier_end) {
                frontier_node_id = overlapping_node_id;
                frontier_end = overlapping_end;
            }
        }
    }

    for (size_t index = 1; index < nodes.size(); ++index) {
        const auto previous_node_id = nodes[index - 1];
        const auto successor_node_id = nodes[index];
        const auto & previous = graph_.event_for_node(previous_node_id);
        const auto & successor = graph_.event_for_node(successor_node_id);
        const auto gap_start = source_dag_index_detail::saturated_add(previous.ts, previous.dur);
        const auto gap_end = successor.ts;
        if (gap_end <= gap_start) continue;
        const auto owned_start = std::max(output.interval_start_us, gap_start);
        const auto owned_end = std::min(output.interval_end_us, gap_end);
        if (owned_end <= owned_start) continue;
        output.owned_gap_slices.push_back(HiCacheCpuGapSlice{
            .owner_node_id = previous_node_id,
            .successor_node_id = successor_node_id,
            .logical_input_id = graph_.node(previous_node_id).gpu_id,
            .gap_start_us = gap_start,
            .gap_end_us = gap_end,
            .owned_start_us = owned_start,
            .owned_end_us = owned_end,
        });
        output.owned_gap_duration_us =
            core::checked_add_u64(output.owned_gap_duration_us, owned_end - owned_start, "HiCache timing-owned CPU gap exceeds uint64 range");
    }

    const auto covered =
        core::checked_add_u64(output.owned_node_duration_us, output.owned_gap_duration_us, "HiCache timing-owned interval exceeds uint64 range");
    constexpr uint64_t timestamp_rounding_tolerance_us = 4;
    if (covered <= output.observed_duration_us) output.uncovered_duration_us = output.observed_duration_us - covered;
    if (partial_node_overlap) output.reason = "timing interval cuts through a retained executable leaf";
    else if (covered > output.observed_duration_us && covered - output.observed_duration_us > timestamp_rounding_tolerance_us)
        output.reason = "same-lane executable intervals overlap inside the timing observation";
    else if (output.uncovered_duration_us > 0) output.reason = "part of the timing interval is outside retained same-lane nodes and CPU gaps";
    else if (!output.start_anchor_node_id || !output.completion_anchor_node_id) output.reason = "timing interval lacks a complete same-lane boundary pair";
    else {
        output.status = "ready";
        output.reason = covered > output.observed_duration_us ? "same pid/tid call containment is complete within four microseconds timestamp rounding"
                                                              : "same pid/tid call containment exactly partitions timing into retained leaves and CPU gaps";
    }
    return output;
}

std::optional<HiCacheTimingIntervalOwnership> HiCacheSourceDagIndex::enclosing_control_interval_ownership(const HiCacheSourceFactNode & fact,
                                                                                                          std::string_view control_event_name) const {
    const auto found = control_intervals_by_name_.find(control_event_name);
    if (found == control_intervals_by_name_.end()) return std::nullopt;
    const auto fact_end_us = source_dag_index_detail::saturated_add(fact.timestamp_us, fact.duration_us);
    const ControlInterval * best = nullptr;
    constexpr uint64_t timestamp_rounding_tolerance_us = 4;
    for (const auto & candidate : found->second) {
        if (candidate.pid != fact.pid || candidate.tid != fact.tid) continue;
        const auto candidate_start_with_tolerance =
            candidate.start_us > timestamp_rounding_tolerance_us ? candidate.start_us - timestamp_rounding_tolerance_us : 0;
        const auto candidate_end_with_tolerance =
            source_dag_index_detail::saturated_add(candidate.end_us, timestamp_rounding_tolerance_us);
        if (candidate_start_with_tolerance > fact.timestamp_us || candidate_end_with_tolerance < fact_end_us) continue;
        const auto candidate_duration = candidate.end_us - candidate.start_us;
        if (best == nullptr || candidate_duration < best->end_us - best->start_us) best = &candidate;
    }
    if (best == nullptr || best->end_us <= best->start_us) return std::nullopt;
    return timing_interval_ownership(best->pid, best->tid, best->start_us, best->end_us - best->start_us);
}

HiCacheDeviceTransferClosure HiCacheSourceDagIndex::device_transfer_closure(const HiCacheSourceFactNode & submission, std::string_view direction) const {
    HiCacheDeviceTransferClosure output;
    if (submission.duration_us == 0 || direction.empty()) {
        output.reason = "device-transfer closure requires a nonzero host submission and explicit direction";
        return output;
    }

    const auto ownership = timing_interval_ownership(submission);
    std::set<int> logical_inputs;
    const auto append_input = [&](std::optional<size_t> node_id) {
        if (node_id && *node_id < graph_.node_count()) logical_inputs.insert(graph_.node(*node_id).gpu_id);
    };
    append_input(ownership.start_anchor_node_id);
    append_input(ownership.completion_anchor_node_id);
    for (size_t node_id : ownership.owned_node_ids) {
        if (node_id < graph_.node_count()) logical_inputs.insert(graph_.node(node_id).gpu_id);
    }
    if (logical_inputs.size() != 1) {
        output.reason = "host submission does not resolve to exactly one logical input";
        return output;
    }

    const auto interval_end = source_dag_index_detail::saturated_add(submission.timestamp_us, submission.duration_us);
    const auto logical_input = *logical_inputs.begin();
    for (const auto & node : graph_.nodes()) {
        if (!node.active || node.is_cpu || node.gpu_id != logical_input) continue;
        const auto & event = graph_.event_for_node(node.id);
        if (event.arg("operation") != direction) continue;
        const auto submit_ts = node.submit_ts > 0 ? node.submit_ts : event.ts;
        if (submit_ts < submission.timestamp_us || submit_ts > interval_end) continue;
        output.transfer_node_ids.push_back(node.id);
        output.transfer_duration_us =
            core::checked_add_u64(output.transfer_duration_us, node.duration, "HiCache source device-transfer duration exceeds uint64 range");
    }
    std::ranges::sort(output.transfer_node_ids, [&](size_t left, size_t right) {
        const auto & lhs = graph_.event_for_node(left);
        const auto & rhs = graph_.event_for_node(right);
        if (lhs.ts != rhs.ts) return lhs.ts < rhs.ts;
        return left < right;
    });
    if (output.transfer_node_ids.empty()) {
        output.status = "no_device_transfer";
        output.reason = "host submission emitted no direction-matched device transfer";
        return output;
    }

    std::set<size_t> completion_nodes;
    std::set<size_t> readiness_joins;
    std::vector<size_t> frontier(output.transfer_node_ids.begin(), output.transfer_node_ids.end());
    std::unordered_set<size_t> visited;
    visited.reserve(frontier.size() * 2);
    while (!frontier.empty()) {
        const auto current = frontier.back();
        frontier.pop_back();
        if (!visited.insert(current).second) continue;

        bool terminal_readiness = false;
        for (size_t edge_index : outgoing_edge_ids(current)) {
            const auto & edge = graph_.edge(edge_index);
            if (!edge.active || edge.dst >= graph_.node_count() || !graph_.node(edge.dst).active) continue;
            if (edge.kind == core::DagEdgeKind::Sync && graph_.node(edge.dst).gpu_id == logical_input
                && graph_.node(edge.dst).lane_id != graph_.node(current).lane_id) {
                completion_nodes.insert(current);
                readiness_joins.insert(edge.dst);
                terminal_readiness = true;
            }
        }
        // A readiness-bearing record is the structural end of this transfer
        // batch.  Do not walk through it into a later operation on the same
        // stream.
        if (terminal_readiness) continue;

        for (size_t edge_index : outgoing_edge_ids(current)) {
            const auto & edge = graph_.edge(edge_index);
            if (!edge.active || edge.dst >= graph_.node_count() || !graph_.node(edge.dst).active) continue;
            if (edge.kind != core::DagEdgeKind::Stream || graph_.node(edge.dst).lane_id != graph_.node(current).lane_id) continue;
            const auto & successor = graph_.node(edge.dst);
            const auto & successor_event = graph_.event_for_node(edge.dst);
            const auto successor_submit_ts = successor.submit_ts > 0 ? successor.submit_ts : successor_event.ts;
            const bool contiguous_record_tail = successor_event.name == "EVENT_RECORD";
            if ((successor_submit_ts < submission.timestamp_us || successor_submit_ts > interval_end) && !contiguous_record_tail) continue;
            frontier.push_back(edge.dst);
        }
    }
    output.completion_node_ids.assign(completion_nodes.begin(), completion_nodes.end());
    output.readiness_join_node_ids.assign(readiness_joins.begin(), readiness_joins.end());
    output.status = output.readiness_join_node_ids.empty() ? "transfer_only" : "ready";
    output.reason = output.readiness_join_node_ids.empty() ? "device transfer is observed but no cross-lane event readiness join is proven"
                                                           : "direction-matched device transfers retain cross-lane event record/wait readiness joins";
    return output;
}

std::vector<HiCacheCpuGapSlice>
    HiCacheSourceDagIndex::project_foreground_gap_across_logical_input_lanes(std::span<const HiCacheCpuGapSlice> source_slices) const {
    if (source_slices.empty()) return {};
    const auto first_owner = source_slices.front().owner_node_id;
    if (first_owner >= graph_.node_count()) return {};
    const auto logical_input_id = graph_.node(first_owner).gpu_id;

    std::vector<std::pair<uint64_t, uint64_t>> intervals;
    intervals.reserve(source_slices.size());
    std::vector<std::string_view> source_lane_keys;
    for (const auto & slice : source_slices) {
        if (slice.owner_node_id >= graph_.node_count() || slice.successor_node_id >= graph_.node_count()) return {};
        const auto & owner = graph_.node(slice.owner_node_id);
        const auto & successor = graph_.node(slice.successor_node_id);
        if (owner.gpu_id != logical_input_id || successor.gpu_id != logical_input_id) return {};
        source_lane_keys.push_back(graph_.node_lane_key(slice.owner_node_id));
        if (slice.owned_end_us > slice.owned_start_us) intervals.emplace_back(slice.owned_start_us, slice.owned_end_us);
    }
    std::ranges::sort(source_lane_keys);
    source_lane_keys.erase(std::unique(source_lane_keys.begin(), source_lane_keys.end()), source_lane_keys.end());
    std::ranges::sort(intervals);
    std::vector<std::pair<uint64_t, uint64_t>> merged;
    for (const auto & interval : intervals) {
        if (!merged.empty() && interval.first <= merged.back().second) merged.back().second = std::max(merged.back().second, interval.second);
        else merged.push_back(interval);
    }

    std::map<std::pair<size_t, size_t>, std::vector<std::pair<uint64_t, uint64_t>>> projected;
    const auto logical_input_lanes = cpu_lane_keys_by_logical_input_.find(logical_input_id);
    if (logical_input_lanes == cpu_lane_keys_by_logical_input_.end()) return {};
    for (const auto & lane_key : logical_input_lanes->second) {
        if (std::ranges::find(source_lane_keys, std::string_view(lane_key)) != source_lane_keys.end()) continue;
        const auto lane = cpu_nodes_by_lane_.find(lane_key);
        if (lane == cpu_nodes_by_lane_.end() || lane->second.size() < 2) continue;
        const auto & nodes = lane->second;
        for (const auto & [interval_start, interval_end] : merged) {
            auto bound = std::lower_bound(nodes.begin(), nodes.end(), interval_start, [&](size_t node_id, uint64_t timestamp) {
                return graph_.event_for_node(node_id).ts < timestamp;
            });
            size_t index = static_cast<size_t>(std::distance(nodes.begin(), bound));
            if (index > 0) --index;
            for (; index + 1 < nodes.size(); ++index) {
                const auto previous_node_id = nodes[index];
                const auto successor_node_id = nodes[index + 1];
                const auto & previous = graph_.event_for_node(previous_node_id);
                const auto & successor = graph_.event_for_node(successor_node_id);
                const auto gap_start = source_dag_index_detail::saturated_add(previous.ts, previous.dur);
                const auto gap_end = successor.ts;
                if (gap_start >= interval_end && successor.ts >= interval_end) break;
                if (gap_end <= gap_start) continue;
                const auto owned_start = std::max(interval_start, gap_start);
                const auto owned_end = std::min(interval_end, gap_end);
                if (owned_end <= owned_start) continue;
                projected[{ previous_node_id, successor_node_id }].emplace_back(owned_start, owned_end);
            }
        }
    }

    std::vector<HiCacheCpuGapSlice> output;
    for (auto & [edge, slices] : projected) {
        std::ranges::sort(slices);
        std::vector<std::pair<uint64_t, uint64_t>> merged_slices;
        for (const auto & slice : slices) {
            if (!merged_slices.empty() && slice.first <= merged_slices.back().second)
                merged_slices.back().second = std::max(merged_slices.back().second, slice.second);
            else merged_slices.push_back(slice);
        }
        const auto & previous = graph_.event_for_node(edge.first);
        const auto & successor = graph_.event_for_node(edge.second);
        const auto gap_start = source_dag_index_detail::saturated_add(previous.ts, previous.dur);
        for (const auto & [owned_start, owned_end] : merged_slices) {
            output.push_back(HiCacheCpuGapSlice{
                .owner_node_id = edge.first,
                .successor_node_id = edge.second,
                .logical_input_id = logical_input_id,
                .gap_start_us = gap_start,
                .gap_end_us = successor.ts,
                .owned_start_us = owned_start,
                .owned_end_us = owned_end,
            });
        }
    }
    std::ranges::sort(output, [](const auto & left, const auto & right) {
        if (left.owned_start_us != right.owned_start_us) return left.owned_start_us < right.owned_start_us;
        if (left.owner_node_id != right.owner_node_id) return left.owner_node_id < right.owner_node_id;
        return left.successor_node_id < right.successor_node_id;
    });
    return output;
}

} // namespace markov::trace_graph::modules::hicache::patch
