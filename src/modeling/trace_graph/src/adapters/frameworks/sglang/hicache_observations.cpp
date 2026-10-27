/**
 * @file
 * @brief Compact source-DAG index implementation.
 */
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"

#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/fact.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_set>

namespace markov::trace_graph::adapters::sglang {

namespace hicache_source_detail {

using Json = nlohmann::json;

uint64_t saturated_add(uint64_t start, uint64_t duration) {
    return duration > std::numeric_limits<uint64_t>::max() - start ? std::numeric_limits<uint64_t>::max() : start + duration;
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

// Window and tail facts share the same observed fields. The caller assigns
// their identity and applies the relevant lifecycle validation.
HiCacheFactObservation fact_record(const core::TraceEvent & event, size_t fact_id) {
    auto metadata = modules::hicache::parse_hicache_fact_metadata(event);
    auto operation_id = event.arg("operation_id");
    if (operation_id.empty() && event.has_arg_key_hint("node_id")) operation_id = event.arg("node_id");

    std::optional<uint64_t> object_node_id;
    if (event.has_arg_key_hint("node_id")) {
        const auto raw_node_id = event.find_arg("node_id");
        if (raw_node_id) object_node_id = core::parse_u64(*raw_node_id);
    }

    return HiCacheFactObservation{
        .node_id = fact_id,
        .event_index = event.index,
        .timestamp_us = event.ts,
        .duration_us = event.dur,
        .pid = event.pid,
        .tid = event.tid,
        .target_id = event.arg("target_id"),
        .phase = event.arg("phase"),
        .fact_class = std::move(metadata.fact_class),
        .fact_role = std::move(metadata.role),
        .request_id = event.arg("request_id"),
        .batch_request_ids = request_ids_arg(event),
        .operation_id = std::move(operation_id),
        .cache_scope = event.arg("cache_scope"),
        .object_node_id = object_node_id,
        .full_path_span = modules::hicache::parse_hicache_token_span_arg(event, "full_path_span"),
        .source_page_size = event.arg_u64("source_page_size", 0),
        .service_item_count = event.arg_u64("service_item_count", 0),
        .storage_existing_page_count = core::parse_u64(event.arg("storage_existing_page_count")),
        .storage_new_page_count = core::parse_u64(event.arg("storage_new_page_count")),
        .token_count = event.arg_u64("token_count", 0),
        .effective_token_count = event.arg_u64("effective_token_count", 0),
        .completed_token_count = event.arg_u64("completed_token_count", 0),
        .completed_token_count_present = event.has_arg_key_hint("completed_token_count"),
        .progress_ready = bool_arg(event, "progress_ready"),
        .host_available_tokens_at_return = core::parse_exact_u64(event.arg("host_available_tokens_at_return")),
        .write_back = bool_arg(event, "write_back"),
        .operation_node_ids = u64_array_arg(event, "operation_node_ids"),
        .page_hashes = string_array_arg(event, "page_hashes"),
    };
}

} // namespace hicache_source_detail

HiCacheSourceIndex::HiCacheSourceIndex(const core::DagGraph & graph) : source_dag::SourceDagIndex(graph) {
    struct PendingControlInterval {
        std::string event_name;
        ControlInterval interval;
    };
    std::unordered_map<std::string, PendingControlInterval> pending_control_intervals;

    for (const auto & node : graph.nodes()) {
        if (!node.is_cpu) {
            const auto direction = graph.event_for_node(node.id).arg("operation");
            if (!direction.empty()) device_nodes_by_direction_[direction].push_back(node.id);
        }
        if (!node.active) continue;
        if (node.is_cpu) {
            const auto & event = graph.event_for_node(node.id);
            if (event.name.starts_with("hicache.control.") && event.name.ends_with(".self") && event.has_arg_key_hint("hicache_control_parent")
                && event.has_arg_key_hint("hicache_control_parent_index")) {
                const auto parent = event.arg("hicache_control_parent");
                const auto parent_index = event.arg("hicache_control_parent_index");
                if (!parent.empty() && !parent_index.empty()) {
                    const auto identity = parent + "\x1f" + event.pid + "\x1f" + event.tid + "\x1f" + parent_index;
                    const auto event_end = hicache_source_detail::saturated_add(event.ts, event.dur);
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

    modules::hicache::HiCacheFactParser source_fact_parser;
    for (const auto & event : graph.hicache_fact_events()) source_fact_parser.observe_token_dictionaries(event);
    for (const auto & event : graph.tail_context_events()) source_fact_parser.observe_token_dictionaries(event);

    for (const auto & event : graph.hicache_fact_events()) {
        if (!hicache_source_detail::fact_candidate(event)) continue;
        try {
            const auto fact_id = event.index;
            auto fact = hicache_source_detail::fact_record(event, fact_id);
            if (fact.fact_role == "cache_lifecycle_commit") {
                // Preserve lifecycle field validation; page identity is computed
                // by the state model, not duplicated in this attribution index.
                (void)source_fact_parser.parse(fact_id, event, std::nullopt);
            }

            const auto fact_index = fact_nodes_.size();
            fact_nodes_.push_back(std::move(fact));
            fact_index_by_node_.emplace(fact_id, fact_index);
            if (!fact_nodes_.back().request_id.empty()) nodes_by_request_[fact_nodes_.back().request_id].push_back(fact_id);
            nodes_by_fact_role_[fact_nodes_.back().fact_role].push_back(fact_id);
            for (const auto & request_id : fact_nodes_.back().batch_request_ids) nodes_by_request_[request_id].push_back(fact_id);
        }
        catch (const std::exception &) {
            // Malformed facts cannot supply attribution evidence.
        }
    }

    const auto tail_offset = modules::hicache::hicache_tail_fact_offset(graph.hicache_fact_events());
    for (const auto & event : graph.tail_context_events()) {
        if (!hicache_source_detail::fact_candidate(event)) continue;
        try {
            const auto tail_fact_index = tail_context_facts_.size();
            if (event.index > std::numeric_limits<size_t>::max() - tail_offset) {
                throw std::overflow_error("HiCache tail fact identity exceeds size_t range");
            }
            const auto tail_fact_node_id = tail_offset + event.index;
            tail_context_facts_.push_back(hicache_source_detail::fact_record(event, tail_fact_node_id));
            tail_fact_index_by_node_.emplace(tail_fact_node_id, tail_fact_index);
        }
        catch (const std::exception &) {
            // Tail context is optional closure evidence; malformed rows cannot prove closure.
        }
    }
}

const HiCacheFactObservation * HiCacheSourceIndex::fact_node(size_t node_id) const {
    const auto it = fact_index_by_node_.find(node_id);
    if (it != fact_index_by_node_.end()) return &fact_nodes_[it->second];
    const auto tail = tail_fact_index_by_node_.find(node_id);
    return tail == tail_fact_index_by_node_.end() ? nullptr : &tail_context_facts_[tail->second];
}

std::span<const size_t> HiCacheSourceIndex::nodes_for_fact_role(std::string_view role) const { return find_nodes(nodes_by_fact_role_, role); }

std::span<const size_t> HiCacheSourceIndex::nodes_for_request(std::string_view request_id) const { return find_nodes(nodes_by_request_, request_id); }

std::optional<source_dag::TimingIntervalOwnership> HiCacheSourceIndex::enclosing_control_interval_ownership(const HiCacheFactObservation & fact,
                                                                                                            std::string_view control_event_name) const {
    const auto found = control_intervals_by_name_.find(control_event_name);
    if (found == control_intervals_by_name_.end()) return std::nullopt;
    const auto fact_end_us = hicache_source_detail::saturated_add(fact.timestamp_us, fact.duration_us);
    const ControlInterval * best = nullptr;
    constexpr uint64_t timestamp_rounding_tolerance_us = 4;
    for (const auto & candidate : found->second) {
        if (candidate.pid != fact.pid || candidate.tid != fact.tid) continue;
        const auto candidate_start_with_tolerance =
            candidate.start_us > timestamp_rounding_tolerance_us ? candidate.start_us - timestamp_rounding_tolerance_us : 0;
        const auto candidate_end_with_tolerance = hicache_source_detail::saturated_add(candidate.end_us, timestamp_rounding_tolerance_us);
        if (candidate_start_with_tolerance > fact.timestamp_us || candidate_end_with_tolerance < fact_end_us) continue;
        const auto candidate_duration = candidate.end_us - candidate.start_us;
        if (best == nullptr || candidate_duration < best->end_us - best->start_us) best = &candidate;
    }
    if (best == nullptr || best->end_us <= best->start_us) return std::nullopt;
    return timing_interval_ownership(best->pid, best->tid, best->start_us, best->end_us - best->start_us);
}

DeviceTransferClosure HiCacheSourceIndex::device_transfer_closure(const HiCacheFactObservation & submission, std::string_view direction) const {
    DeviceTransferClosure output;
    if (submission.duration_us == 0 || direction.empty()) {
        output.reason = "device-transfer closure requires a nonzero host submission and explicit direction";
        return output;
    }

    const auto ownership = timing_interval_ownership(submission);
    std::set<int> logical_inputs;
    const auto append_input = [&](std::optional<size_t> node_id) {
        if (node_id && *node_id < graph().node_count()) logical_inputs.insert(graph().node(*node_id).gpu_id);
    };
    append_input(ownership.start_anchor_node_id);
    append_input(ownership.completion_anchor_node_id);
    for (size_t node_id : ownership.owned_node_ids) {
        if (node_id < graph().node_count()) logical_inputs.insert(graph().node(node_id).gpu_id);
    }
    if (logical_inputs.size() != 1) {
        output.reason = "host submission does not resolve to exactly one logical input";
        return output;
    }

    const auto interval_end = hicache_source_detail::saturated_add(submission.timestamp_us, submission.duration_us);
    const auto logical_input = *logical_inputs.begin();
    for (const auto id : find_nodes(device_nodes_by_direction_, direction)) {
        const auto & node = graph().node(id);
        if (!node.active || node.is_cpu || node.gpu_id != logical_input) continue;
        const auto & event = graph().event_for_node(node.id);
        if (event.arg("operation") != direction) continue;
        if (!device_submitted_in_call(id, submission.pid, submission.tid, submission.timestamp_us, interval_end)) continue;
        output.transfer_node_ids.push_back(node.id);
        output.transfer_duration_us =
            core::checked_add_u64(output.transfer_duration_us, node.duration, "HiCache source device-transfer duration exceeds uint64 range");
    }
    std::ranges::sort(output.transfer_node_ids, [&](size_t left, size_t right) {
        const auto & lhs = graph().event_for_node(left);
        const auto & rhs = graph().event_for_node(right);
        if (lhs.ts != rhs.ts) return lhs.ts < rhs.ts;
        return left < right;
    });
    if (output.transfer_node_ids.empty()) {
        output.status = "no_device_transfer";
        output.reason = "no direction-matched device transfer could be attributed to the host caller";
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
            const auto & edge = graph().edge(edge_index);
            if (!edge.active || edge.dst >= graph().node_count() || !graph().node(edge.dst).active) continue;
            if (edge.kind == core::DagEdgeKind::Sync && graph().node(edge.dst).gpu_id == logical_input
                && graph().node(edge.dst).lane_id != graph().node(current).lane_id) {
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
            const auto & edge = graph().edge(edge_index);
            if (!edge.active || edge.dst >= graph().node_count() || !graph().node(edge.dst).active) continue;
            if (edge.kind != core::DagEdgeKind::Stream || graph().node(edge.dst).lane_id != graph().node(current).lane_id) continue;
            if (!device_submitted_in_call(edge.dst, submission.pid, submission.tid, submission.timestamp_us, interval_end)) continue;
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

} // namespace markov::trace_graph::adapters::sglang
