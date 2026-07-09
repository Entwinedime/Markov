/**
 * @file
 * @brief HiCache SimulationModule 包装层实现。
 */
#include "markov/trace_graph/modules/hicache/hicache_module.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache {

namespace hicache_module_detail {

constexpr std::string_view kModuleName = "HiCacheModule";

#ifdef DEBUG
constexpr std::string_view kSemanticHiCacheShapePatchModel = "semantic_hicache_intent_physical_best_effort_cleanup_patch_v28";
constexpr std::string_view kCostWorkUnitModel = "hicache_query_device_host_storage_prefill_target_wait_cleanup_units_v10";
constexpr std::string_view kIntervalAttributionModel = "cpu_interval_window_effect_attribution_diagnostic_v12";
constexpr std::string_view kResultPressureEffect = "hicache_result_pressure";
constexpr uint64_t kMaxCpuIntervalNs = 1'000'000'000'000ull;
constexpr double kEffectScaleEpsilon = 0.000001;
constexpr double kSourceResidueWaitScaleFloor = 0.05;

struct DagPatchStats {
    uint64_t mutation_count = 0;
    uint64_t policy_decision_count = 0;
    uint64_t transition_count = 0;
    uint64_t scoped_node_count = 0;
    uint64_t scoped_interval_node_count = 0;
    uint64_t scoped_interval_ns = 0;
    uint64_t shape_edge_count = 0;
    uint64_t skipped_reverse_shape_edge_count = 0;
    uint64_t window_count = 0;
    uint64_t window_interval_node_count = 0;
    uint64_t window_interval_ns = 0;
    uint64_t physical_window_count = 0;
    uint64_t physical_node_count = 0;
    uint64_t physical_interval_node_count = 0;
    uint64_t physical_interval_ns = 0;
    uint64_t physical_shape_edge_count = 0;
    uint64_t physical_duration_mutation_count = 0;
    uint64_t physical_source_duration_ns = 0;
    uint64_t physical_mutated_duration_ns = 0;
    int64_t physical_duration_delta_ns = 0;
    uint64_t target_wait_carrier_count = 0;
    uint64_t target_wait_carrier_duration_ns = 0;
    uint64_t source_wait_gap_mutation_count = 0;
    uint64_t source_wait_gap_interval_node_count = 0;
    uint64_t source_wait_gap_source_interval_ns = 0;
    uint64_t source_wait_gap_mutated_interval_ns = 0;
    int64_t source_wait_gap_interval_delta_ns = 0;
    uint64_t source_best_effort_cleanup_gap_mutation_count = 0;
    uint64_t source_best_effort_cleanup_gap_interval_node_count = 0;
    uint64_t source_best_effort_cleanup_gap_source_interval_ns = 0;
    uint64_t source_best_effort_cleanup_gap_mutated_interval_ns = 0;
    int64_t source_best_effort_cleanup_gap_interval_delta_ns = 0;
    uint64_t cost_mutation_count = 0;
    uint64_t cost_interval_node_count = 0;
    uint64_t cost_source_interval_ns = 0;
    uint64_t cost_mutated_interval_ns = 0;
    int64_t cost_interval_delta_ns = 0;
    double cost_interval_scale = 1.0;
    uint64_t cost_unattributed_interval_node_count = 0;
    uint64_t cost_unattributed_interval_ns = 0;
    uint64_t cost_source_work_units = 0;
    uint64_t cost_target_work_units = 0;
    uint64_t cost_source_intent_units = 0;
    uint64_t cost_target_intent_units = 0;
    double cost_source_blocking_result_score = 0.0;
    double cost_target_blocking_result_score = 0.0;
    double cost_source_wait_score = 0.0;
    double cost_target_wait_score = 0.0;
    double cost_source_wait_residue_scale = 1.0;
    double cost_target_wait_carrier_score = 0.0;
    double cost_source_best_effort_cleanup_scale = 1.0;
    double cost_result_cleanup_policy_scale = 1.0;
    double cost_result_effect_scale = 1.0;
    double cost_prefill_compute_scale = 1.0;
    double cost_result_io_blocking_scale = 1.0;
    bool source_timeout_wait_relief = false;
    double source_wait_gap_base_scale = 1.0;
    double source_wait_gap_device_host_scale = 1.0;
    bool source_best_effort_cleanup_relief = false;
    double source_best_effort_cleanup_gap_scale = 1.0;
    std::string cost_work_unit_model;
    std::string cost_interval_attribution_model;
    std::map<std::string, uint64_t> intent_by_policy_area;
    std::map<std::string, uint64_t> intent_by_role;
    std::map<std::string, uint64_t> shape_edges_by_kind;
    std::map<std::string, uint64_t> shape_edges_by_impact;
    std::map<std::string, uint64_t> scoped_interval_by_role;
    std::map<std::string, uint64_t> window_interval_by_kind;
    std::map<std::string, uint64_t> physical_nodes_by_kind;
    std::map<std::string, uint64_t> physical_nodes_by_impact;
    std::map<std::string, uint64_t> physical_interval_by_kind;
    std::map<std::string, uint64_t> physical_interval_by_impact;
    std::map<std::string, uint64_t> physical_shape_edges_by_kind;
    std::map<std::string, uint64_t> physical_shape_edges_by_impact;
    std::map<std::string, uint64_t> physical_source_duration_by_effect;
    std::map<std::string, uint64_t> physical_mutated_duration_by_effect;
    std::map<std::string, uint64_t> physical_duration_node_count_by_effect;
    std::map<std::string, double> physical_duration_scale_by_effect;
    std::map<std::string, uint64_t> cost_source_interval_by_kind;
    std::map<std::string, uint64_t> cost_mutated_interval_by_kind;
    std::map<std::string, uint64_t> cost_source_interval_by_effect;
    std::map<std::string, uint64_t> cost_mutated_interval_by_effect;
    std::map<std::string, uint64_t> cost_source_interval_by_impact;
    std::map<std::string, uint64_t> cost_mutated_interval_by_impact;
    std::map<std::string, double> cost_interval_scale_by_effect;
    std::map<std::string, uint64_t> cost_interval_node_count_by_effect;
    std::map<std::string, uint64_t> cost_interval_node_count_by_impact;
    std::map<std::string, uint64_t> cost_source_work_units_by_effect;
    std::map<std::string, uint64_t> cost_target_work_units_by_effect;
    std::map<std::string, uint64_t> cost_source_work_units_by_impact;
    std::map<std::string, uint64_t> cost_target_work_units_by_impact;
    std::map<std::string, uint64_t> cost_source_work_units_by_effect_policy_area;
    std::map<std::string, uint64_t> cost_target_work_units_by_effect_policy_area;
    std::map<std::string, uint64_t> cost_source_work_units_by_impact_policy_area;
    std::map<std::string, uint64_t> cost_target_work_units_by_impact_policy_area;
    std::map<std::string, uint64_t> cost_source_intent_units_by_effect_policy_area;
    std::map<std::string, uint64_t> cost_target_intent_units_by_effect_policy_area;
    std::map<std::string, uint64_t> cost_source_intent_units_by_impact_policy_area;
    std::map<std::string, uint64_t> cost_target_intent_units_by_impact_policy_area;
    std::map<std::string, uint64_t> cost_source_work_units_by_policy_area;
    std::map<std::string, uint64_t> cost_target_work_units_by_policy_area;
    std::map<std::string, uint64_t> cost_source_work_units_by_role;
    std::map<std::string, uint64_t> cost_target_work_units_by_role;
    std::map<std::string, double> cost_model_scale_by_effect;
    std::vector<std::string> warnings;
};

struct CostWorkUnits {
    uint64_t page_units = 0;
    uint64_t intent_units = 0;
    std::map<std::string, uint64_t> by_effect;
    std::map<std::string, uint64_t> by_impact;
    std::map<std::string, uint64_t> by_effect_policy_area;
    std::map<std::string, uint64_t> by_impact_policy_area;
    std::map<std::string, uint64_t> intent_by_effect_policy_area;
    std::map<std::string, uint64_t> intent_by_impact_policy_area;
    std::map<std::string, uint64_t> by_policy_area;
    std::map<std::string, uint64_t> by_role;
};

struct RequestAnchors {
    std::vector<size_t> lookup_nodes;
    std::vector<size_t> prefetch_nodes;
    std::vector<size_t> extend_nodes;
    std::vector<size_t> commit_nodes;
    bool result_io_blocking = false;
};

struct CpuIntervalEntry {
    size_t node_id = 0;
    size_t dst_node_id = 0;
    uint64_t src_ts = 0;
    uint64_t dst_ts = 0;
    uint64_t interval_ns = 0;
};

struct IntervalAttribution {
    std::unordered_set<std::string> window_kinds;
    std::unordered_set<std::string> effect_kinds;
};

struct CpuIntervalLaneIndex {
    std::vector<CpuIntervalEntry> intervals;
    std::vector<uint64_t> prefix_ns;
};

struct PhysicalNodeEntry {
    size_t node_id = 0;
    uint64_t ts = 0;
    uint64_t interval_ns = 0;
    std::string kind;
};

struct PhysicalWindowEndpoint {
    size_t first = 0;
    size_t last = 0;
    bool seen = false;
};

struct CriticalPredecessor {
    size_t node_id = std::numeric_limits<size_t>::max();
    core::DagEdgeKind edge_kind = core::DagEdgeKind::Sequential;
};

std::string lower_copy(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool contains(std::string_view haystack, std::string_view needle) { return haystack.find(needle) != std::string_view::npos; }

uint64_t read_u64_attr(const core::DagNode & node, const std::string & key, uint64_t fallback = 0) {
    const auto it = node.attrs.find(key);
    if (it == node.attrs.end()) return fallback;
    try {
        return std::stoull(it->second);
    }
    catch (...) {
        return fallback;
    }
}

uint64_t ceil_div_u64(uint64_t value, uint64_t divisor) {
    if (divisor == 0 || value == 0) return 0;
    return value / divisor + (value % divisor == 0 ? 0 : 1);
}

void add_role(std::unordered_map<size_t, std::unordered_set<std::string>> & roles_by_node, size_t node_id, const std::string & role) {
    roles_by_node[node_id].insert(role.empty() ? std::string{ "unknown" } : role);
}

void merge_roles(std::unordered_map<size_t, std::unordered_set<std::string>> & roles_by_node, size_t node_id, const std::unordered_set<std::string> & roles) {
    auto & target = roles_by_node[node_id];
    if (roles.empty()) {
        target.insert("unknown");
        return;
    }
    target.insert(roles.begin(), roles.end());
}

std::string transition_request_key(const model::HiCacheStateTransition & transition) {
    if (transition.request_id.empty()) return {};
    if (transition.cache_scope.empty()) return transition.request_id;
    return transition.cache_scope + ":" + transition.request_id;
}

bool is_blocking_result_io_transition(std::string_view kind) {
    return kind == "enqueue_write_through_backup" || kind == "enqueue_storage_backup" || kind == "commit_host_storage_backup"
        || kind == "complete_storage_backup";
}

void note_transition_effect(RequestAnchors & anchors, const model::HiCacheStateTransition & transition) {
    if (transition.role == "cache_lifecycle_commit" && is_blocking_result_io_transition(transition.kind)) anchors.result_io_blocking = true;
}

void add_anchor(RequestAnchors & anchors, const std::string & role, size_t node_id) {
    if (role == "cache_lookup_input") {
        anchors.lookup_nodes.push_back(node_id);
        return;
    }
    if (role == "prefetch_candidate_anchor") {
        anchors.prefetch_nodes.push_back(node_id);
        return;
    }
    if (role == "cache_extend_input") {
        anchors.extend_nodes.push_back(node_id);
        return;
    }
    if (role == "cache_lifecycle_commit") anchors.commit_nodes.push_back(node_id);
}

void sort_unique_nodes_by_ts(const core::DagGraph & graph, std::vector<size_t> & nodes) {
    std::sort(nodes.begin(), nodes.end(), [&](size_t lhs, size_t rhs) {
        const auto & lhs_event = graph.event_for_node(lhs);
        const auto & rhs_event = graph.event_for_node(rhs);
        if (lhs_event.ts != rhs_event.ts) return lhs_event.ts < rhs_event.ts;
        return lhs < rhs;
    });
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
}

void normalize_request_anchors(const core::DagGraph & graph, RequestAnchors & anchors) {
    sort_unique_nodes_by_ts(graph, anchors.lookup_nodes);
    sort_unique_nodes_by_ts(graph, anchors.prefetch_nodes);
    sort_unique_nodes_by_ts(graph, anchors.extend_nodes);
    sort_unique_nodes_by_ts(graph, anchors.commit_nodes);
}

size_t first_node_at_or_after(const core::DagGraph & graph, const std::vector<size_t> & nodes, uint64_t ts) {
    const auto it = std::lower_bound(nodes.begin(), nodes.end(), ts, [&](size_t node_id, uint64_t value) { return graph.event_for_node(node_id).ts < value; });
    return it == nodes.end() ? graph.node_count() : *it;
}

bool shape_edge_forward(const core::DagGraph & graph, size_t src, size_t dst) {
    if (src >= graph.node_count() || dst >= graph.node_count() || src == dst) return false;
    const auto & src_event = graph.event_for_node(src);
    const auto & dst_event = graph.event_for_node(dst);
    if (src_event.ts != dst_event.ts) return src_event.ts < dst_event.ts;
    return src < dst;
}

std::string shape_edge_key(size_t src, size_t dst, const std::string & kind) { return std::to_string(src) + "->" + std::to_string(dst) + ":" + kind; }

std::string shape_edge_attr_key(size_t src) { return "hicache_shape_edge_from:" + std::to_string(src); }

std::string direct_shape_impact_category(std::string_view window_kind);
bool any_effect_shape_enabled(const DagPatchStats & stats, std::initializer_list<std::string_view> effects);
uint64_t target_wait_carrier_budget_ns(const model::HiCacheSummary & target_summary, const model::HiCacheSummary * source_summary,
                                       const DagPatchStats & stats);

bool add_shape_edge(DagPatchStats & stats, core::DagGraph & graph, std::unordered_set<std::string> & emitted_edges, size_t src, size_t dst,
                    const std::string & kind) {
    if (src >= graph.node_count() || dst >= graph.node_count() || src == dst) return false;
    if (!shape_edge_forward(graph, src, dst)) {
        ++stats.skipped_reverse_shape_edge_count;
        return false;
    }
    auto key = shape_edge_key(src, dst, kind);
    if (!emitted_edges.insert(std::move(key)).second) return false;
    graph.add_edge(src, dst, core::DagEdgeKind::HiCache);
    graph.set_node_attr(dst, shape_edge_attr_key(src), kind);
    ++stats.mutation_count;
    ++stats.shape_edge_count;
    stats.shape_edges_by_kind[kind]++;
    return true;
}

uint64_t window_cpu_interval_ns(const core::DagGraph & graph, const std::unordered_map<std::string, CpuIntervalLaneIndex> & lanes, size_t begin_node_id,
                                size_t end_node_id) {
    if (begin_node_id >= graph.node_count() || end_node_id >= graph.node_count()) return 0;
    const auto & begin_node = graph.node(begin_node_id);
    const auto & end_node = graph.node(end_node_id);
    const auto & begin_event = graph.event_for_node(begin_node_id);
    const auto & end_event = graph.event_for_node(end_node_id);
    if (end_event.ts <= begin_event.ts || begin_node.lane_key != end_node.lane_key) return 0;
    const auto lane_it = lanes.find(begin_node.lane_key);
    if (lane_it == lanes.end()) return 0;
    const auto & intervals = lane_it->second.intervals;
    const auto & prefix = lane_it->second.prefix_ns;
    const auto begin_it =
        std::lower_bound(intervals.begin(), intervals.end(), begin_event.ts, [](const auto & entry, uint64_t value) { return entry.src_ts < value; });
    const auto end_it =
        std::upper_bound(intervals.begin(), intervals.end(), end_event.ts, [](uint64_t value, const auto & entry) { return value < entry.src_ts; });
    const auto begin_index = static_cast<size_t>(std::distance(intervals.begin(), begin_it));
    const auto end_index = static_cast<size_t>(std::distance(intervals.begin(), end_it));
    return end_index <= begin_index ? 0 : prefix[end_index] - prefix[begin_index];
}

void add_target_wait_carrier_node(DagPatchStats & stats, core::DagGraph & graph, std::unordered_set<std::string> & emitted_edges, size_t begin_node_id,
                                  size_t end_node_id, uint64_t duration_ns) {
    if (begin_node_id >= graph.node_count() || end_node_id >= graph.node_count() || duration_ns == 0) return;
    const auto begin_node = graph.node(begin_node_id);
    const auto begin_event = graph.event_for_node(begin_node_id);
    const auto end_event = graph.event_for_node(end_node_id);
    if (end_event.ts <= begin_event.ts + 1) return;

    core::TraceEvent event;
    event.index = graph.events().size();
    event.name = "hicache_target_wait_pressure";
    event.cat = "hicache";
    event.ph = "X";
    event.ts = begin_event.ts + std::max<uint64_t>(1, (end_event.ts - begin_event.ts) / 2);
    event.dur = duration_ns;
    event.pid = begin_event.pid;
    event.tid = begin_event.tid;
    event.set_arg("domain", "hicache");
    event.set_arg("event_kind", "target_wait_pressure");
    event.set_arg("target", "result_blocking");
    event.set_arg("duration_ns", std::to_string(duration_ns));

    graph.mutable_events().push_back(std::move(event));
    const auto carrier_node = graph.add_node(graph.events().size() - 1, true, begin_node.lane_key);
    graph.set_node_attr(carrier_node, "role", "hicache_target_wait_pressure");
    graph.set_node_attr(carrier_node, "time", std::to_string(duration_ns));
    graph.set_node_attr(carrier_node, "hicache_synthetic", "target_wait_pressure");

    if (add_shape_edge(stats, graph, emitted_edges, begin_node_id, carrier_node, "target_wait_pressure_begin"))
        stats.shape_edges_by_impact["hicache_query_result_effect"]++;
    if (add_shape_edge(stats, graph, emitted_edges, carrier_node, end_node_id, "target_wait_pressure_end"))
        stats.shape_edges_by_impact["hicache_query_result_effect"]++;
    ++stats.target_wait_carrier_count;
    stats.target_wait_carrier_duration_ns += duration_ns;
    ++stats.mutation_count;
}

void add_target_wait_carrier(DagPatchStats & stats, core::DagGraph & graph, const std::unordered_map<std::string, CpuIntervalLaneIndex> & lanes,
                             std::unordered_map<std::string, RequestAnchors> & anchors_by_request, const model::HiCacheSummary & summary,
                             const model::HiCacheSummary * source_summary) {
    const auto budget_ns = target_wait_carrier_budget_ns(summary, source_summary, stats);
    if (budget_ns == 0) return;
    size_t best_begin = graph.node_count();
    size_t best_end = graph.node_count();
    uint64_t best_interval = 0;
    for (auto & [_, anchors] : anchors_by_request) {
        normalize_request_anchors(graph, anchors);
        for (const auto extend_node : anchors.extend_nodes) {
            const auto commit_node = first_node_at_or_after(graph, anchors.commit_nodes, graph.event_for_node(extend_node).ts);
            const auto interval_ns = window_cpu_interval_ns(graph, lanes, extend_node, commit_node);
            if (interval_ns <= best_interval) continue;
            best_interval = interval_ns;
            best_begin = extend_node;
            best_end = commit_node;
        }
    }
    if (best_begin >= graph.node_count() || best_end >= graph.node_count()) {
        stats.warnings.push_back("semantic_patch_target_wait_carrier_window_empty");
        return;
    }
    stats.cost_target_wait_carrier_score = stats.cost_target_wait_score - stats.cost_source_wait_score;
    std::unordered_set<std::string> emitted_edges;
    add_target_wait_carrier_node(stats, graph, emitted_edges, best_begin, best_end, budget_ns);
}

void add_request_shape_edges(DagPatchStats & stats, core::DagGraph & graph, std::unordered_map<std::string, RequestAnchors> & anchors_by_request) {
    std::unordered_set<std::string> emitted_edges;
    for (auto & [_, anchors] : anchors_by_request) {
        normalize_request_anchors(graph, anchors);
        for (const auto prefetch_node : anchors.prefetch_nodes) {
            if (!any_effect_shape_enabled(stats, { "host_storage_io" })) continue;
            const auto extend_node = first_node_at_or_after(graph, anchors.extend_nodes, graph.event_for_node(prefetch_node).ts);
            if (add_shape_edge(stats, graph, emitted_edges, prefetch_node, extend_node, "prefetch_to_extend"))
                stats.shape_edges_by_impact[direct_shape_impact_category("prefetch_to_extend")]++;
        }
        for (const auto lookup_node : anchors.lookup_nodes) {
            if (!any_effect_shape_enabled(stats, { "hicache_query", "device_host_io" })) continue;
            const auto extend_node = first_node_at_or_after(graph, anchors.extend_nodes, graph.event_for_node(lookup_node).ts);
            if (add_shape_edge(stats, graph, emitted_edges, lookup_node, extend_node, "lookup_to_extend"))
                stats.shape_edges_by_impact[direct_shape_impact_category("lookup_to_extend")]++;
        }
    }
}

bool is_hicache_probe_event(const core::TraceEvent & event) {
    if (event.cat == "hicache" || event.name.starts_with("hicache_")) return true;
    const auto raw = event.args_json_view();
    if (raw.find("domain") == std::string_view::npos) return false;
    const auto domain = event.arg("domain");
    return domain == "hicache" || domain == "python_probe";
}

std::string normalize_effect_kind(std::string kind) {
    if (kind == "scheduler_radix_cpu_work") return "hicache_query";
    if (kind == "hicache_query_result") return "query_result_effect_unknown";
    return kind;
}

bool is_effect_kind(std::string_view kind) {
    return kind == "hicache_query" || kind == "device_host_io" || kind == "host_storage_io" || kind == "llm_prefill_compute_effect"
        || kind == "query_result_effect_unknown" || kind == kResultPressureEffect;
}

bool is_query_result_effect_kind(std::string_view kind) {
    return kind == "device_host_io" || kind == "host_storage_io" || kind == "llm_prefill_compute_effect" || kind == "query_result_effect_unknown"
        || kind == kResultPressureEffect;
}

std::string impact_category_for_effect(std::string_view effect) {
    if (effect == "hicache_query") return "hicache_query";
    if (is_query_result_effect_kind(effect)) return "hicache_query_result_effect";
    return "unknown";
}

std::string direct_shape_impact_category(std::string_view window_kind) {
    if (window_kind == "lookup_to_extend") return "hicache_query";
    if (window_kind == "prefetch_to_extend") return "hicache_query_result_effect";
    if (window_kind == "extend_to_commit" || window_kind == "extend_to_commit_compute") return "hicache_query_result_effect";
    return "unknown";
}

std::string shape_edge_impact_category(std::string_view kind) {
    if (kind.starts_with("physical_")) {
        const auto separator = kind.rfind(':');
        if (separator != std::string_view::npos && separator + 1 < kind.size())
            return impact_category_for_effect(normalize_effect_kind(std::string{ kind.substr(separator + 1) }));
    }
    return direct_shape_impact_category(kind);
}

uint64_t prefill_compute_tokens(const model::HiCacheSummary & summary) {
    uint64_t tokens = 0;
    for (const auto & decision : summary.policy_decision_trace) {
        if (!decision.accepted || decision.policy_area != "device_allocation") continue;
        if (decision.extend_tokens > 0) {
            tokens += decision.extend_tokens;
            continue;
        }
        if (decision.requested_tokens > 0) {
            tokens += decision.requested_tokens;
            continue;
        }
        const auto page_size = summary.resolved_policy.page_size > 0 ? summary.resolved_policy.page_size : summary.target_config.page_size;
        const auto normalized_page_size = std::max<uint64_t>(page_size, 1);
        if (decision.requested_pages > 0) tokens += decision.requested_pages * normalized_page_size;
        else if (decision.allocated_pages > 0) tokens += decision.allocated_pages * normalized_page_size;
        else if (decision.candidate_pages > 0) tokens += decision.candidate_pages * normalized_page_size;
        else if (decision.hit_pages > 0) tokens += decision.hit_pages * normalized_page_size;
        else if (!decision.pages.empty()) tokens += static_cast<uint64_t>(decision.pages.size()) * normalized_page_size;
    }
    return tokens;
}

bool same_hicache_cost_config(const model::HiCacheSummary & lhs, const model::HiCacheSummary & rhs) {
    const auto & lhs_policy = lhs.resolved_policy;
    const auto & rhs_policy = rhs.resolved_policy;
    return lhs_policy.page_size == rhs_policy.page_size && lhs_policy.l1_capacity_pages == rhs_policy.l1_capacity_pages
        && lhs_policy.l2_capacity_pages == rhs_policy.l2_capacity_pages && lhs_policy.write_policy == rhs_policy.write_policy
        && lhs_policy.write_through_threshold == rhs_policy.write_through_threshold && lhs_policy.prefetch_policy == rhs_policy.prefetch_policy
        && lhs_policy.prefetch_threshold_pages == rhs_policy.prefetch_threshold_pages
        && lhs_policy.prefetch_capacity_limit_pages == rhs_policy.prefetch_capacity_limit_pages
        && lhs_policy.prefetch_timeout_configured == rhs_policy.prefetch_timeout_configured
        && lhs_policy.prefetch_timeout_base_sec == rhs_policy.prefetch_timeout_base_sec
        && lhs_policy.prefetch_timeout_per_ki_token_sec == rhs_policy.prefetch_timeout_per_ki_token_sec
        && lhs_policy.prefetch_timeout_max_sec == rhs_policy.prefetch_timeout_max_sec;
}

double clamped_scale(double value, double min_value, double max_value) {
    if (!std::isfinite(value) || value <= 0.0) return 1.0;
    return std::clamp(value, min_value, max_value);
}

uint64_t map_value_or_zero(const std::map<std::string, uint64_t> & values, const std::string & key) {
    const auto it = values.find(key);
    return it == values.end() ? uint64_t{ 0 } : it->second;
}

uint64_t cleanup_policy_work_units(const std::map<std::string, uint64_t> & by_effect_policy_area) {
    return map_value_or_zero(by_effect_policy_area, "device_host_io:device_allocator")
        + map_value_or_zero(by_effect_policy_area, "device_host_io:host_cleanup")
        + map_value_or_zero(by_effect_policy_area, "device_host_io:write_policy");
}

double ratio_scale(uint64_t source_units, uint64_t target_units, double min_value = 0.05, double max_value = 5.0) {
    if (source_units == 0 || target_units == 0) return 1.0;
    return clamped_scale(static_cast<double>(target_units) / static_cast<double>(source_units), min_value, max_value);
}

double wait_threshold_token_score(const model::HiCacheSummary & summary) {
    const auto tokens = summary.resolved_policy.prefetch_threshold_tokens > 0
        ? summary.resolved_policy.prefetch_threshold_tokens
        : summary.resolved_policy.prefetch_threshold_pages * std::max<uint64_t>(summary.resolved_policy.page_size, 1);
    return static_cast<double>(tokens) / 1024.0;
}

double result_wait_policy_score(const model::HiCacheSummary & summary) {
    const auto policy = lower_copy(summary.resolved_policy.prefetch_policy);
    if (policy == "timeout") {
        const auto timeout_budget_sec = std::max(summary.resolved_policy.prefetch_timeout_base_sec,
                                                summary.resolved_policy.prefetch_timeout_max_sec);
        if (timeout_budget_sec <= 0.0) return 0.0;
        return timeout_budget_sec * wait_threshold_token_score(summary);
    }
    if (policy == "wait_complete") return wait_threshold_token_score(summary);
    if (policy == "best_effort") {
        const auto capacity_pages = std::max<uint64_t>(summary.resolved_policy.prefetch_capacity_limit_pages, 1);
        const auto threshold_pages = summary.resolved_policy.prefetch_threshold_pages;
        if (threshold_pages == 0) return 0.0;
        const auto page_pressure = static_cast<double>(threshold_pages) / static_cast<double>(capacity_pages);
        return page_pressure * wait_threshold_token_score(summary);
    }
    return 0.0;
}

uint64_t seconds_to_ns(double seconds) {
    if (seconds <= 0.0) return 0;
    const auto ns = seconds * 1'000'000'000.0;
    if (ns >= static_cast<double>(std::numeric_limits<uint64_t>::max())) return std::numeric_limits<uint64_t>::max();
    return static_cast<uint64_t>(std::llround(ns));
}

uint64_t target_timeout_wait_carrier_unit_ns(const model::HiCacheSummary & summary) {
    const auto policy = lower_copy(summary.resolved_policy.prefetch_policy);
    if (policy != "timeout" || summary.resolved_policy.prefetch_timeout_per_ki_token_sec <= 0.0) return 0;
    const auto threshold_tokens = wait_threshold_token_score(summary);
    const auto threshold_pages = static_cast<double>(summary.resolved_policy.prefetch_threshold_pages);
    const auto threshold_fraction = threshold_pages > 0.0 ? threshold_pages / (threshold_pages + 1.0) : 0.0;
    return seconds_to_ns(summary.resolved_policy.prefetch_timeout_per_ki_token_sec * threshold_tokens * threshold_fraction);
}

uint64_t target_wait_carrier_budget_ns(const model::HiCacheSummary & target_summary, const model::HiCacheSummary * source_summary,
                                       const DagPatchStats & stats) {
    if (source_summary == nullptr || same_hicache_cost_config(target_summary, *source_summary)) return 0;
    if (stats.cost_target_wait_score <= stats.cost_source_wait_score + kEffectScaleEpsilon) return 0;
    const auto unit_ns = target_timeout_wait_carrier_unit_ns(target_summary);
    if (unit_ns == 0 || stats.cost_target_wait_score <= kEffectScaleEpsilon) return 0;
    const auto score_delta = stats.cost_target_wait_score - stats.cost_source_wait_score;
    return static_cast<uint64_t>(std::llround(static_cast<double>(unit_ns) * score_delta / stats.cost_target_wait_score));
}

bool can_leave_source_wait_residue(const model::HiCacheSummary & summary) {
    const auto policy = lower_copy(summary.resolved_policy.prefetch_policy);
    if (policy == "timeout") return std::max(summary.resolved_policy.prefetch_timeout_base_sec, summary.resolved_policy.prefetch_timeout_max_sec) > 0.0;
    return policy == "best_effort";
}

double cost_model_scale_for_effect(const DagPatchStats & stats, std::string_view effect) {
    const auto it = stats.cost_model_scale_by_effect.find(std::string{ effect });
    return it == stats.cost_model_scale_by_effect.end() ? 1.0 : it->second;
}

bool effect_shape_enabled(const DagPatchStats & stats, std::string_view effect) {
    if (stats.cost_model_scale_by_effect.empty()) return true;
    const auto effect_key = std::string{ effect };
    if (map_value_or_zero(stats.cost_source_work_units_by_effect, effect_key) != map_value_or_zero(stats.cost_target_work_units_by_effect, effect_key))
        return true;
    const auto scale = cost_model_scale_for_effect(stats, effect);
    return std::abs(scale - 1.0) > kEffectScaleEpsilon;
}

bool any_effect_shape_enabled(const DagPatchStats & stats, std::initializer_list<std::string_view> effects) {
    for (const auto effect : effects) {
        if (effect_shape_enabled(stats, effect)) return true;
    }
    return false;
}

bool physical_shape_effect_enabled(const DagPatchStats & stats, std::string_view effect) {
    if (!effect_shape_enabled(stats, effect)) return false;
    if (stats.cost_model_scale_by_effect.empty()) return true;
    const auto effect_key = std::string{ effect };
    const auto source_units = map_value_or_zero(stats.cost_source_work_units_by_effect, effect_key);
    const auto target_units = map_value_or_zero(stats.cost_target_work_units_by_effect, effect_key);
    return target_units >= source_units;
}

bool source_wait_residue_relief_enabled(const DagPatchStats & stats) {
    return stats.cost_source_wait_residue_scale < 1.0 - kEffectScaleEpsilon;
}

bool source_best_effort_cleanup_relief_enabled(const DagPatchStats & stats) {
    return stats.source_best_effort_cleanup_relief && stats.source_best_effort_cleanup_gap_scale < 1.0 - kEffectScaleEpsilon;
}

bool attribution_has_window(const IntervalAttribution & attribution, std::string_view window_kind) {
    return attribution.window_kinds.find(std::string{ window_kind }) != attribution.window_kinds.end();
}

bool source_wait_gap_effect(const DagPatchStats & stats, std::string_view effect) {
    if (effect == "hicache_query" || effect == kResultPressureEffect || effect == "query_result_effect_unknown") return true;
    return stats.source_timeout_wait_relief && (effect == "device_host_io" || effect == "llm_prefill_compute_effect");
}

double source_wait_gap_effect_scale(const DagPatchStats & stats, std::string_view effect) {
    if (effect == "device_host_io") return stats.source_wait_gap_device_host_scale;
    return stats.source_wait_gap_base_scale;
}

std::optional<double> source_wait_gap_interval_scale(const DagPatchStats & stats, const IntervalAttribution & attribution) {
    if (!source_wait_residue_relief_enabled(stats)) return std::nullopt;
    const auto result_related_window =
        attribution_has_window(attribution, "extend_to_commit") || attribution_has_window(attribution, "extend_to_commit_compute");
    if (!result_related_window) return std::nullopt;
    std::optional<double> scale;
    for (const auto & effect : attribution.effect_kinds) {
        if (!source_wait_gap_effect(stats, effect)) continue;
        const auto effect_scale = source_wait_gap_effect_scale(stats, effect);
        scale = scale ? std::max(*scale, effect_scale) : effect_scale;
    }
    return scale;
}

bool source_best_effort_cleanup_effect(const DagPatchStats & stats, std::string_view effect) {
    if (effect == "hicache_query" || effect == "device_host_io" || effect == "host_storage_io" || effect == kResultPressureEffect
        || effect == "query_result_effect_unknown")
        return true;
    return effect == "llm_prefill_compute_effect" && stats.cost_prefill_compute_scale < 1.0 - kEffectScaleEpsilon;
}

std::optional<double> source_best_effort_cleanup_gap_interval_scale(const DagPatchStats & stats, const IntervalAttribution & attribution) {
    if (!source_best_effort_cleanup_relief_enabled(stats)) return std::nullopt;
    const auto result_related_window =
        attribution_has_window(attribution, "extend_to_commit") || attribution_has_window(attribution, "extend_to_commit_compute");
    if (!result_related_window) return std::nullopt;
    for (const auto & effect : attribution.effect_kinds) {
        if (source_best_effort_cleanup_effect(stats, effect)) return stats.source_best_effort_cleanup_gap_scale;
    }
    return std::nullopt;
}

bool physical_shape_kind_allowed_for_window(const DagPatchStats & stats, std::string_view window_kind, std::string_view raw_kind) {
    const auto kind = normalize_effect_kind(std::string{ raw_kind });
    if (window_kind == "prefetch_to_extend") return kind == "host_storage_io" && physical_shape_effect_enabled(stats, kind);
    if (window_kind == "lookup_to_extend") return (kind == "hicache_query" || kind == "device_host_io") && physical_shape_effect_enabled(stats, kind);
    if ((window_kind == "extend_to_commit" || window_kind == "extend_to_commit_compute") && source_wait_residue_relief_enabled(stats)) return false;
    if (window_kind == "extend_to_commit") return is_query_result_effect_kind(kind) && physical_shape_effect_enabled(stats, kind);
    if (window_kind == "extend_to_commit_compute") return kind == "llm_prefill_compute_effect" && physical_shape_effect_enabled(stats, kind);
    return is_effect_kind(kind) && physical_shape_effect_enabled(stats, kind);
}

bool physical_duration_kind_allowed_for_window(const DagPatchStats & stats, std::string_view window_kind, std::string_view raw_kind) {
    const auto kind = normalize_effect_kind(std::string{ raw_kind });
    if (window_kind == "prefetch_to_extend") return kind == "host_storage_io" && effect_shape_enabled(stats, kind);
    if (window_kind == "lookup_to_extend") return (kind == "hicache_query" || kind == "device_host_io") && effect_shape_enabled(stats, kind);
    if (window_kind == "extend_to_commit") return is_query_result_effect_kind(kind) && effect_shape_enabled(stats, kind);
    if (window_kind == "extend_to_commit_compute") return kind == "llm_prefill_compute_effect" && effect_shape_enabled(stats, kind);
    return is_effect_kind(kind) && effect_shape_enabled(stats, kind);
}

bool interval_effect_allowed_for_window(const DagPatchStats & stats, std::string_view window_kind, std::string_view raw_effect) {
    const auto effect = normalize_effect_kind(std::string{ raw_effect });
    if (window_kind == "prefetch_to_extend") return effect == "host_storage_io" && effect_shape_enabled(stats, effect);
    if (window_kind == "lookup_to_extend") return (effect == "hicache_query" || effect == "device_host_io") && effect_shape_enabled(stats, effect);
    if (window_kind == "extend_to_commit") return is_query_result_effect_kind(effect) && effect_shape_enabled(stats, effect);
    if (window_kind == "extend_to_commit_compute")
        return (effect == "hicache_query" || effect == "llm_prefill_compute_effect" || effect == kResultPressureEffect)
            && effect_shape_enabled(stats, effect);
    return is_effect_kind(effect) && effect_shape_enabled(stats, effect);
}

std::string classify_hicache_effect_probe(const core::TraceEvent & event) {
    const auto text = lower_copy(event.name + " " + event.cat + " " + event.arg("target") + " " + event.arg("event_kind"));
    if (contains(text, "ready_to_load_host_cache") || contains(text, "load_host_cache") || contains(text, "loadback")) return "device_host_io";
    if (contains(text, "writeback_io") || contains(text, "writeback_enqueue") || contains(text, "flush_write_through") || contains(text, "prefetch_progress")
        || contains(text, "prefetch_intent") || contains(text, "prefetch_decision")) {
        return "host_storage_io";
    }
    if (contains(text, "prefill_admission")) return "llm_prefill_compute_effect";
    return {};
}

std::string fallback_effect_for_window(std::string_view window_kind) {
    if (window_kind == "prefetch_to_extend") return "host_storage_io";
    if (window_kind == "lookup_to_extend") return "hicache_query";
    if (window_kind == "extend_to_commit") return "query_result_effect_unknown";
    if (window_kind == "extend_to_commit_compute") return "hicache_query";
    return "hicache_query";
}

std::string classify_physical_node(const core::TraceEvent & event) {
    if (is_hicache_probe_event(event)) return classify_hicache_effect_probe(event);
    const auto text = lower_copy(event.name + " " + event.cat);

    if (contains(text, "match_prefix") || contains(text, "prepare_for_extend") || contains(text, "_prefetch_kvcache") || contains(text, "radix"))
        return "scheduler_radix_cpu_work";
    if (contains(text, "pagedattention") || contains(text, "paged_attention") || contains(text, "matmul") || contains(text, "rmsnorm")
        || contains(text, "swiglu") || contains(text, "reshapeandcache") || contains(text, "reshape_cache") || contains(text, "split_qkv")
        || contains(text, "computing")) {
        return "llm_prefill_compute_effect";
    }
    if (contains(text, "inplacecopy") || contains(text, "copy_") || contains(text, "_to_copy") || contains(text, "aclrtmemcpy")) return "device_host_io";
    if ((contains(text, "prefetch") && (contains(text, "storage") || contains(text, "read"))) || contains(text, "prefetch_read")) return "host_storage_io";
    if (contains(text, "loadback") || contains(text, "load_back")) return "device_host_io";
    if (contains(text, "writeback") || contains(text, "write_back") || contains(text, "ack")) return "host_storage_io";
    if (contains(text, "cleanup") || contains(text, "release") || contains(text, "drain")) return "device_host_io";
    if (contains(text, "backup") && contains(text, "host")) return "device_host_io";
    if (contains(text, "alloc") || contains(text, "evict") || contains(text, "revoke")) return "device_host_io";
    if (contains(text, "wait_event") || contains(text, "waitevent") || contains(text, "timeout") || contains(text, "prefetch")) return "hicache_query";
    return {};
}

std::string interval_effect_kind(const core::DagGraph & graph, const CpuIntervalEntry & interval, const std::string & window_kind) {
    const auto src_kind =
        interval.node_id < graph.node_count() ? normalize_effect_kind(classify_physical_node(graph.event_for_node(interval.node_id))) : std::string{};
    const auto dst_kind =
        interval.dst_node_id < graph.node_count() ? normalize_effect_kind(classify_physical_node(graph.event_for_node(interval.dst_node_id))) : std::string{};
    if (is_effect_kind(dst_kind)) {
        if (dst_kind == "llm_prefill_compute_effect" || src_kind.empty() || src_kind == "hicache_query" || src_kind == dst_kind) return dst_kind;
    }
    if (is_effect_kind(src_kind)) return src_kind;
    if (is_effect_kind(dst_kind)) return dst_kind;
    return fallback_effect_for_window(window_kind);
}

std::vector<PhysicalNodeEntry> build_physical_node_index(const core::DagGraph & graph) {
    std::vector<PhysicalNodeEntry> nodes;
    nodes.reserve(graph.node_count() / 8);
    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        auto kind = classify_physical_node(event);
        if (kind.empty()) continue;
        nodes.push_back(PhysicalNodeEntry{
            .node_id = node.id,
            .ts = event.ts,
            .interval_ns = read_u64_attr(node, "cpuinterval", 0),
            .kind = std::move(kind),
        });
    }
    std::sort(nodes.begin(), nodes.end(), [](const auto & lhs, const auto & rhs) {
        if (lhs.ts != rhs.ts) return lhs.ts < rhs.ts;
        return lhs.node_id < rhs.node_id;
    });
    return nodes;
}

void record_physical_node_stats(DagPatchStats & stats, const PhysicalNodeEntry & entry, std::unordered_set<size_t> & attributed_nodes) {
    if (!attributed_nodes.insert(entry.node_id).second) return;
    const auto impact = impact_category_for_effect(normalize_effect_kind(entry.kind));
    ++stats.physical_node_count;
    stats.physical_nodes_by_kind[entry.kind]++;
    stats.physical_nodes_by_impact[impact]++;
    if (entry.interval_ns == 0 || entry.interval_ns > kMaxCpuIntervalNs) return;
    ++stats.physical_interval_node_count;
    stats.physical_interval_ns += entry.interval_ns;
    stats.physical_interval_by_kind[entry.kind] += entry.interval_ns;
    stats.physical_interval_by_impact[impact] += entry.interval_ns;
}

void add_physical_edge(DagPatchStats & stats, core::DagGraph & graph, std::unordered_set<std::string> & emitted_edges, size_t src, size_t dst,
                       const std::string & window_kind, const std::string & physical_kind) {
    const auto edge_kind = "physical_" + window_kind + ":" + physical_kind;
    if (!add_shape_edge(stats, graph, emitted_edges, src, dst, edge_kind)) return;
    const auto impact = impact_category_for_effect(normalize_effect_kind(physical_kind));
    ++stats.physical_shape_edge_count;
    stats.physical_shape_edges_by_kind[physical_kind]++;
    stats.physical_shape_edges_by_impact[impact]++;
    stats.shape_edges_by_impact[impact]++;
}

void add_physical_window_shape(DagPatchStats & stats, core::DagGraph & graph, const std::vector<PhysicalNodeEntry> & physical_nodes,
                               std::unordered_set<size_t> & attributed_nodes, std::unordered_set<std::string> & emitted_edges, size_t begin_node_id,
                               size_t end_node_id, const std::string & window_kind) {
    if (begin_node_id >= graph.node_count() || end_node_id >= graph.node_count()) return;
    const auto & begin_event = graph.event_for_node(begin_node_id);
    const auto & end_event = graph.event_for_node(end_node_id);
    if (end_event.ts <= begin_event.ts) return;

    std::map<std::string, PhysicalWindowEndpoint> endpoints_by_kind;
    const auto begin_it =
        std::lower_bound(physical_nodes.begin(), physical_nodes.end(), begin_event.ts, [](const auto & entry, uint64_t value) { return entry.ts < value; });
    for (auto it = begin_it; it != physical_nodes.end() && it->ts <= end_event.ts; ++it) {
        if (!physical_shape_kind_allowed_for_window(stats, window_kind, it->kind)) continue;
        record_physical_node_stats(stats, *it, attributed_nodes);
        auto & endpoint = endpoints_by_kind[it->kind];
        if (!endpoint.seen) {
            endpoint.first = it->node_id;
            endpoint.seen = true;
        }
        endpoint.last = it->node_id;
    }
    if (endpoints_by_kind.empty()) return;
    ++stats.physical_window_count;
    for (const auto & [physical_kind, endpoint] : endpoints_by_kind) {
        add_physical_edge(stats, graph, emitted_edges, begin_node_id, endpoint.first, window_kind + "_begin", physical_kind);
        add_physical_edge(stats, graph, emitted_edges, endpoint.last, end_node_id, window_kind + "_end", physical_kind);
    }
}

void add_request_physical_shape_edges(DagPatchStats & stats, core::DagGraph & graph, const std::vector<PhysicalNodeEntry> & physical_nodes,
                                      std::unordered_map<std::string, RequestAnchors> & anchors_by_request) {
    std::unordered_set<size_t> attributed_nodes;
    std::unordered_set<std::string> emitted_edges;
    for (auto & [_, anchors] : anchors_by_request) {
        normalize_request_anchors(graph, anchors);
        for (const auto prefetch_node : anchors.prefetch_nodes) {
            const auto extend_node = first_node_at_or_after(graph, anchors.extend_nodes, graph.event_for_node(prefetch_node).ts);
            add_physical_window_shape(stats, graph, physical_nodes, attributed_nodes, emitted_edges, prefetch_node, extend_node, "prefetch_to_extend");
        }
        for (const auto lookup_node : anchors.lookup_nodes) {
            const auto extend_node = first_node_at_or_after(graph, anchors.extend_nodes, graph.event_for_node(lookup_node).ts);
            add_physical_window_shape(stats, graph, physical_nodes, attributed_nodes, emitted_edges, lookup_node, extend_node, "lookup_to_extend");
        }
        for (const auto extend_node : anchors.extend_nodes) {
            const auto commit_node = first_node_at_or_after(graph, anchors.commit_nodes, graph.event_for_node(extend_node).ts);
            const auto window_kind = anchors.result_io_blocking ? "extend_to_commit" : "extend_to_commit_compute";
            add_physical_window_shape(stats, graph, physical_nodes, attributed_nodes, emitted_edges, extend_node, commit_node, window_kind);
        }
    }
}

double physical_duration_scale_for_effect(const DagPatchStats & stats, std::string_view raw_effect) {
    const auto effect = normalize_effect_kind(std::string{ raw_effect });
    double scale = 1.0;
    if (effect == "llm_prefill_compute_effect") {
        scale = stats.cost_prefill_compute_scale;
    }
    else if (effect == "device_host_io" || effect == "host_storage_io" || effect == "hicache_query") {
        scale = cost_model_scale_for_effect(stats, effect);
    }
    else {
        return 1.0;
    }
    /*
     * 物理 duration mutation 只表达“target 做得更少”时可从 source 图中解除的工作。
     * target 做得更多时，source trace 没有对应新增物理节点，不能靠拉长已有节点拟合。
     */
    return std::min(scale, 1.0);
}

void record_physical_duration_stats(DagPatchStats & stats, const std::string & effect, uint64_t old_duration, uint64_t new_duration) {
    const auto normalized_effect = normalize_effect_kind(effect);
    ++stats.physical_duration_node_count_by_effect[normalized_effect];
    stats.physical_source_duration_ns += old_duration;
    stats.physical_mutated_duration_ns += new_duration;
    stats.physical_duration_delta_ns += static_cast<int64_t>(new_duration) - static_cast<int64_t>(old_duration);
    stats.physical_source_duration_by_effect[normalized_effect] += old_duration;
    stats.physical_mutated_duration_by_effect[normalized_effect] += new_duration;
}

void mutate_physical_duration_window(DagPatchStats & stats, core::DagGraph & graph, const std::vector<PhysicalNodeEntry> & physical_nodes,
                                     std::unordered_set<size_t> & seen_nodes, size_t begin_node_id, size_t end_node_id,
                                     const std::string & window_kind) {
    if (begin_node_id >= graph.node_count() || end_node_id >= graph.node_count()) return;
    const auto & begin_event = graph.event_for_node(begin_node_id);
    const auto & end_event = graph.event_for_node(end_node_id);
    if (end_event.ts <= begin_event.ts) return;

    const auto begin_it =
        std::lower_bound(physical_nodes.begin(), physical_nodes.end(), begin_event.ts, [](const auto & entry, uint64_t value) { return entry.ts < value; });
    for (auto it = begin_it; it != physical_nodes.end() && it->ts <= end_event.ts; ++it) {
        if (!seen_nodes.insert(it->node_id).second) continue;
        if (!physical_duration_kind_allowed_for_window(stats, window_kind, it->kind)) continue;
        const auto effect = normalize_effect_kind(it->kind);
        const auto old_duration = graph.node(it->node_id).duration;
        if (old_duration == 0 || old_duration > kMaxCpuIntervalNs) continue;
        const auto scale = physical_duration_scale_for_effect(stats, effect);
        auto new_duration = old_duration;
        if (std::abs(scale - 1.0) > kEffectScaleEpsilon) {
            new_duration = static_cast<uint64_t>(std::llround(static_cast<double>(old_duration) * scale));
            if (new_duration > kMaxCpuIntervalNs) new_duration = kMaxCpuIntervalNs;
            if (new_duration != old_duration) {
                graph.set_node_duration(it->node_id, new_duration);
                ++stats.physical_duration_mutation_count;
            }
        }
        record_physical_duration_stats(stats, effect, old_duration, new_duration);
    }
}

void mutate_request_physical_durations(DagPatchStats & stats, core::DagGraph & graph, const std::vector<PhysicalNodeEntry> & physical_nodes,
                                       std::unordered_map<std::string, RequestAnchors> & anchors_by_request) {
    std::unordered_set<size_t> seen_nodes;
    for (auto & [_, anchors] : anchors_by_request) {
        normalize_request_anchors(graph, anchors);
        for (const auto prefetch_node : anchors.prefetch_nodes) {
            const auto extend_node = first_node_at_or_after(graph, anchors.extend_nodes, graph.event_for_node(prefetch_node).ts);
            mutate_physical_duration_window(stats, graph, physical_nodes, seen_nodes, prefetch_node, extend_node, "prefetch_to_extend");
        }
        for (const auto lookup_node : anchors.lookup_nodes) {
            const auto extend_node = first_node_at_or_after(graph, anchors.extend_nodes, graph.event_for_node(lookup_node).ts);
            mutate_physical_duration_window(stats, graph, physical_nodes, seen_nodes, lookup_node, extend_node, "lookup_to_extend");
        }
        for (const auto extend_node : anchors.extend_nodes) {
            const auto commit_node = first_node_at_or_after(graph, anchors.commit_nodes, graph.event_for_node(extend_node).ts);
            const auto window_kind = anchors.result_io_blocking ? "extend_to_commit" : "extend_to_commit_compute";
            mutate_physical_duration_window(stats, graph, physical_nodes, seen_nodes, extend_node, commit_node, window_kind);
        }
    }

    for (const auto & [effect, source_ns] : stats.physical_source_duration_by_effect) {
        if (source_ns == 0) continue;
        const auto mutated_it = stats.physical_mutated_duration_by_effect.find(effect);
        const auto mutated_ns = mutated_it == stats.physical_mutated_duration_by_effect.end() ? uint64_t{ 0 } : mutated_it->second;
        stats.physical_duration_scale_by_effect[effect] = static_cast<double>(mutated_ns) / static_cast<double>(source_ns);
    }
}

std::unordered_map<std::string, CpuIntervalLaneIndex> build_cpu_interval_lane_index(const core::DagGraph & graph) {
    std::unordered_map<std::string, CpuIntervalLaneIndex> lanes;
    for (const auto & edge : graph.edges()) {
        if (edge.kind != core::DagEdgeKind::Sequential || edge.src >= graph.node_count() || edge.dst >= graph.node_count()) continue;
        const auto & src_node = graph.node(edge.src);
        const auto & dst_node = graph.node(edge.dst);
        if (!src_node.is_cpu || !dst_node.is_cpu || src_node.lane_key != dst_node.lane_key) continue;
        const auto interval = read_u64_attr(src_node, "cpuinterval", 0);
        if (interval == 0 || interval > kMaxCpuIntervalNs) continue;
        lanes[src_node.lane_key].intervals.push_back(CpuIntervalEntry{
            .node_id = edge.src,
            .dst_node_id = edge.dst,
            .src_ts = graph.event_for_node(edge.src).ts,
            .dst_ts = graph.event_for_node(edge.dst).ts,
            .interval_ns = interval,
        });
    }
    for (auto & [_, lane] : lanes) {
        std::sort(lane.intervals.begin(), lane.intervals.end(), [](const auto & lhs, const auto & rhs) {
            if (lhs.src_ts != rhs.src_ts) return lhs.src_ts < rhs.src_ts;
            return lhs.node_id < rhs.node_id;
        });
        lane.prefix_ns.clear();
        lane.prefix_ns.reserve(lane.intervals.size() + 1);
        lane.prefix_ns.push_back(0);
        for (const auto & interval : lane.intervals) { lane.prefix_ns.push_back(lane.prefix_ns.back() + interval.interval_ns); }
    }
    return lanes;
}

void add_window_interval_stats(DagPatchStats & stats, const core::DagGraph & graph, const std::unordered_map<std::string, CpuIntervalLaneIndex> & lanes,
                               size_t begin_node_id, size_t end_node_id, const std::string & kind) {
    if (begin_node_id >= graph.node_count() || end_node_id >= graph.node_count()) return;
    const auto & begin_node = graph.node(begin_node_id);
    const auto & end_node = graph.node(end_node_id);
    const auto & begin_event = graph.event_for_node(begin_node_id);
    const auto & end_event = graph.event_for_node(end_node_id);
    if (end_event.ts <= begin_event.ts) return;
    ++stats.window_count;
    if (begin_node.lane_key != end_node.lane_key) {
        stats.warnings.push_back("semantic_patch_window_cross_lane:" + kind);
        return;
    }
    const auto lane_it = lanes.find(begin_node.lane_key);
    if (lane_it == lanes.end()) return;
    const auto & intervals = lane_it->second.intervals;
    const auto & prefix = lane_it->second.prefix_ns;
    const auto begin_it =
        std::lower_bound(intervals.begin(), intervals.end(), begin_event.ts, [](const auto & entry, uint64_t value) { return entry.src_ts < value; });
    const auto end_it =
        std::upper_bound(intervals.begin(), intervals.end(), end_event.ts, [](uint64_t value, const auto & entry) { return value < entry.src_ts; });
    const auto begin_index = static_cast<size_t>(std::distance(intervals.begin(), begin_it));
    const auto end_index = static_cast<size_t>(std::distance(intervals.begin(), end_it));
    if (end_index <= begin_index) return;
    const auto interval_ns = prefix[end_index] - prefix[begin_index];
    const auto interval_count = end_index - begin_index;
    stats.window_interval_node_count += interval_count;
    stats.window_interval_ns += interval_ns;
    stats.window_interval_by_kind[kind] += interval_ns;
}

void add_request_window_stats(DagPatchStats & stats, const core::DagGraph & graph, const std::unordered_map<std::string, CpuIntervalLaneIndex> & lanes,
                              std::unordered_map<std::string, RequestAnchors> & anchors_by_request) {
    for (auto & [_, anchors] : anchors_by_request) {
        normalize_request_anchors(graph, anchors);
        for (const auto prefetch_node : anchors.prefetch_nodes) {
            const auto extend_node = first_node_at_or_after(graph, anchors.extend_nodes, graph.event_for_node(prefetch_node).ts);
            add_window_interval_stats(stats, graph, lanes, prefetch_node, extend_node, "prefetch_to_extend");
        }
        for (const auto lookup_node : anchors.lookup_nodes) {
            const auto extend_node = first_node_at_or_after(graph, anchors.extend_nodes, graph.event_for_node(lookup_node).ts);
            add_window_interval_stats(stats, graph, lanes, lookup_node, extend_node, "lookup_to_extend");
        }
        for (const auto extend_node : anchors.extend_nodes) {
            const auto commit_node = first_node_at_or_after(graph, anchors.commit_nodes, graph.event_for_node(extend_node).ts);
            const auto window_kind = anchors.result_io_blocking ? "extend_to_commit" : "extend_to_commit_compute";
            add_window_interval_stats(stats, graph, lanes, extend_node, commit_node, window_kind);
        }
    }
}

uint64_t decision_page_work_units(const HiCachePolicyDecisionRecord & decision, uint64_t page_size) {
    if (!decision.accepted) return 0;
    if (decision.requested_pages > 0) return decision.requested_pages;
    if (decision.allocated_pages > 0) return decision.allocated_pages;
    if (decision.candidate_pages > 0) return decision.candidate_pages;
    if (decision.hit_pages > 0) return decision.hit_pages;
    if (!decision.pages.empty()) return static_cast<uint64_t>(decision.pages.size());
    return ceil_div_u64(decision.requested_tokens, page_size);
}

uint64_t decision_page_intent_units(const HiCachePolicyDecisionRecord & decision, uint64_t page_size) {
    if (decision.requested_pages > 0) return decision.requested_pages;
    if (decision.allocated_pages > 0) return decision.allocated_pages;
    if (decision.candidate_pages > 0) return decision.candidate_pages;
    if (decision.hit_pages > 0) return decision.hit_pages;
    if (!decision.pages.empty()) return static_cast<uint64_t>(decision.pages.size());
    return ceil_div_u64(decision.requested_tokens, page_size);
}

uint64_t decision_effect_work_units(const HiCachePolicyDecisionRecord & decision, std::string_view effect, uint64_t page_size) {
    if (effect == "llm_prefill_compute_effect") {
        if (decision.extend_tokens > 0) return decision.extend_tokens;
        if (decision.requested_tokens > 0) return decision.requested_tokens;
        return decision_page_work_units(decision, page_size) * std::max<uint64_t>(page_size, 1);
    }
    if (effect == "device_host_io" || effect == "host_storage_io") return decision_page_work_units(decision, page_size) * std::max<uint64_t>(page_size, 1);
    return decision_page_work_units(decision, page_size);
}

std::string stable_key_part(std::string value) { return value.empty() ? std::string{ "unknown" } : std::move(value); }

std::string composite_key(std::string lhs, std::string rhs) { return stable_key_part(std::move(lhs)) + ":" + stable_key_part(std::move(rhs)); }

std::vector<std::string> decision_effect_kinds(const HiCachePolicyDecisionRecord & decision) {
    std::vector<std::string> effects;
    const auto policy_area = lower_copy(decision.policy_area);
    const auto policy_name = lower_copy(decision.policy_name);
    const auto decision_name = lower_copy(decision.decision);
    const auto text = policy_area + " " + policy_name + " " + decision_name;

    if (policy_area == "prefetch_enqueue" || policy_area == "prefetch_cache_extend_boundary") {
        effects.push_back("hicache_query");
        effects.push_back("host_storage_io");
    }
    if (policy_area == "device_allocator" || policy_area == "request_lifecycle" || contains(text, "loadback") || contains(text, "device_allocator")
        || contains(text, "device_allocation")) {
        effects.push_back("device_host_io");
    }
    if (policy_area == "device_allocation") {
        effects.push_back("device_host_io");
        effects.push_back("llm_prefill_compute_effect");
    }
    if (policy_area == "host_allocation" || policy_area == "host_cleanup") {
        if (contains(text, "prefetch") || contains(text, "storage")) effects.push_back("host_storage_io");
        else effects.push_back("device_host_io");
    }
    if (policy_area == "write_policy") {
        if (contains(text, "storage")) effects.push_back("host_storage_io");
        else effects.push_back("device_host_io");
    }
    if (effects.empty()) effects.push_back("query_result_effect_unknown");
    std::sort(effects.begin(), effects.end());
    effects.erase(std::unique(effects.begin(), effects.end()), effects.end());
    return effects;
}

CostWorkUnits policy_decision_work_units(const model::HiCacheSummary & summary) {
    CostWorkUnits units;
    const auto page_size = summary.resolved_policy.page_size > 0 ? summary.resolved_policy.page_size : summary.target_config.page_size;
    for (const auto & decision : summary.policy_decision_trace) {
        const auto policy_area = stable_key_part(decision.policy_area);
        const auto page_units = decision_page_work_units(decision, page_size);
        const auto intent_units = decision_page_intent_units(decision, page_size);
        const auto effects = decision_effect_kinds(decision);
        if (intent_units > 0) {
            units.intent_units += intent_units;
            for (const auto & raw_effect : effects) {
                const auto effect = normalize_effect_kind(raw_effect);
                const auto impact = impact_category_for_effect(effect);
                units.intent_by_effect_policy_area[composite_key(effect, policy_area)] += intent_units;
                units.intent_by_impact_policy_area[composite_key(impact, policy_area)] += intent_units;
            }
        }
        for (const auto & raw_effect : effects) {
            const auto effect = normalize_effect_kind(raw_effect);
            const auto effect_units = decision_effect_work_units(decision, effect, page_size);
            if (effect_units == 0) continue;
            const auto impact = impact_category_for_effect(effect);
            units.by_effect[effect] += effect_units;
            units.by_impact[impact] += effect_units;
            units.by_effect_policy_area[composite_key(effect, policy_area)] += effect_units;
            units.by_impact_policy_area[composite_key(impact, policy_area)] += effect_units;
        }
        if (page_units == 0) continue;
        units.page_units += page_units;
        units.by_policy_area[policy_area] += page_units;
        units.by_role[decision.role.empty() ? "unknown" : decision.role] += page_units;
    }
    return units;
}

void record_cost_work_units(const model::HiCacheSummary & target_summary, const model::HiCacheSummary * source_summary, DagPatchStats & stats) {
    stats.cost_work_unit_model = std::string{ kCostWorkUnitModel };
    const auto target_units = policy_decision_work_units(target_summary);
    stats.cost_target_blocking_result_score = static_cast<double>(map_value_or_zero(target_units.by_effect, "device_host_io")
                                                                  + map_value_or_zero(target_units.by_effect, "host_storage_io"));
    stats.cost_target_wait_score = result_wait_policy_score(target_summary);
    stats.cost_target_work_units = target_units.page_units;
    stats.cost_target_intent_units = target_units.intent_units;
    stats.cost_target_work_units_by_effect = target_units.by_effect;
    stats.cost_target_work_units_by_impact = target_units.by_impact;
    stats.cost_target_work_units_by_effect_policy_area = target_units.by_effect_policy_area;
    stats.cost_target_work_units_by_impact_policy_area = target_units.by_impact_policy_area;
    stats.cost_target_intent_units_by_effect_policy_area = target_units.intent_by_effect_policy_area;
    stats.cost_target_intent_units_by_impact_policy_area = target_units.intent_by_impact_policy_area;
    stats.cost_target_work_units_by_policy_area = target_units.by_policy_area;
    stats.cost_target_work_units_by_role = target_units.by_role;

    if (source_summary == nullptr) {
        stats.warnings.push_back("semantic_patch_cost_source_summary_missing");
        return;
    }

    const auto source_units = policy_decision_work_units(*source_summary);
    stats.cost_source_blocking_result_score = static_cast<double>(map_value_or_zero(source_units.by_effect, "device_host_io")
                                                                  + map_value_or_zero(source_units.by_effect, "host_storage_io"));
    stats.cost_source_wait_score = result_wait_policy_score(*source_summary);
    stats.cost_source_work_units = source_units.page_units;
    stats.cost_source_intent_units = source_units.intent_units;
    stats.cost_source_work_units_by_effect = source_units.by_effect;
    stats.cost_source_work_units_by_impact = source_units.by_impact;
    stats.cost_source_work_units_by_effect_policy_area = source_units.by_effect_policy_area;
    stats.cost_source_work_units_by_impact_policy_area = source_units.by_impact_policy_area;
    stats.cost_source_intent_units_by_effect_policy_area = source_units.intent_by_effect_policy_area;
    stats.cost_source_intent_units_by_impact_policy_area = source_units.intent_by_impact_policy_area;
    stats.cost_source_work_units_by_policy_area = source_units.by_policy_area;
    stats.cost_source_work_units_by_role = source_units.by_role;
    stats.cost_result_cleanup_policy_scale =
        ratio_scale(cleanup_policy_work_units(source_units.by_effect_policy_area), cleanup_policy_work_units(target_units.by_effect_policy_area), 0.05, 5.0);
    if (source_units.page_units == 0 || target_units.page_units == 0) {
        stats.warnings.push_back("semantic_patch_cost_work_units_empty");
        return;
    }
    const auto source_prefetch_policy = lower_copy(source_summary->resolved_policy.prefetch_policy);
    const auto target_prefetch_policy = lower_copy(target_summary.resolved_policy.prefetch_policy);
    if (can_leave_source_wait_residue(*source_summary) && stats.cost_source_wait_score > stats.cost_target_wait_score) {
        stats.cost_source_wait_residue_scale =
            clamped_scale((stats.cost_target_wait_score + kSourceResidueWaitScaleFloor)
                              / (stats.cost_source_wait_score + kSourceResidueWaitScaleFloor),
                          0.05,
                          1.0);
        stats.source_timeout_wait_relief = source_prefetch_policy == "timeout";
        stats.source_wait_gap_base_scale = stats.cost_source_wait_residue_scale;
        stats.source_wait_gap_device_host_scale = stats.cost_source_wait_residue_scale;
        if (stats.source_timeout_wait_relief && target_prefetch_policy == "wait_complete") {
            stats.source_wait_gap_base_scale = 0.0;
            stats.source_wait_gap_device_host_scale = 0.0;
        }
        else if (stats.source_timeout_wait_relief && target_prefetch_policy == "best_effort") {
            const auto source_page_size = std::max<uint64_t>(source_summary->resolved_policy.page_size, 1);
            const auto target_page_size = std::max<uint64_t>(target_summary.resolved_policy.page_size, 1);
            const auto page_ratio = static_cast<double>(target_page_size) / static_cast<double>(source_page_size);
            stats.source_wait_gap_device_host_scale = clamped_scale(page_ratio, stats.cost_source_wait_residue_scale, 1.0);
        }
    }
    if (same_hicache_cost_config(target_summary, *source_summary)) {
        stats.cost_result_effect_scale = 1.0;
        stats.cost_prefill_compute_scale = 1.0;
        stats.cost_result_io_blocking_scale = 1.0;
        stats.cost_source_wait_residue_scale = 1.0;
        stats.cost_target_blocking_result_score = stats.cost_source_blocking_result_score;
        stats.cost_target_wait_score = stats.cost_source_wait_score;
    }
    else if (stats.cost_source_blocking_result_score > 0.0) {
        stats.cost_result_io_blocking_scale = clamped_scale(stats.cost_target_blocking_result_score / stats.cost_source_blocking_result_score, 0.05, 5.0);
        stats.cost_result_effect_scale = stats.cost_result_io_blocking_scale;
    }
    const auto source_prefill_tokens = prefill_compute_tokens(*source_summary);
    const auto target_prefill_tokens = prefill_compute_tokens(target_summary);
    if (!same_hicache_cost_config(target_summary, *source_summary)) {
        stats.cost_prefill_compute_scale = ratio_scale(source_prefill_tokens, target_prefill_tokens, 0.05, 5.0);
    }
    stats.cost_model_scale_by_effect["hicache_query"] =
        same_hicache_cost_config(target_summary, *source_summary)
            ? 1.0
            : ratio_scale(map_value_or_zero(source_units.by_effect, "hicache_query"), map_value_or_zero(target_units.by_effect, "hicache_query"), 0.05, 5.0);
    stats.cost_model_scale_by_effect["device_host_io"] =
        ratio_scale(map_value_or_zero(source_units.by_effect, "device_host_io"), map_value_or_zero(target_units.by_effect, "device_host_io"), 0.05, 5.0);
    stats.cost_model_scale_by_effect["host_storage_io"] =
        ratio_scale(map_value_or_zero(source_units.by_effect, "host_storage_io"), map_value_or_zero(target_units.by_effect, "host_storage_io"), 0.05, 5.0);
    stats.cost_model_scale_by_effect["llm_prefill_compute_effect"] = stats.cost_prefill_compute_scale;
    stats.cost_result_effect_scale = std::max(stats.cost_result_io_blocking_scale, stats.cost_prefill_compute_scale);
    stats.cost_model_scale_by_effect["query_result_effect_unknown"] = stats.cost_result_effect_scale;
    stats.cost_model_scale_by_effect[std::string{ kResultPressureEffect }] = std::min(1.0, stats.cost_result_io_blocking_scale);
    if (source_prefetch_policy == "best_effort" && target_prefetch_policy != "best_effort") {
        stats.source_best_effort_cleanup_relief = true;
        stats.cost_source_best_effort_cleanup_scale = std::min(
            1.0, cost_model_scale_for_effect(stats, "hicache_query") * cost_model_scale_for_effect(stats, "host_storage_io"));
        const auto cleanup_io_scale = std::min(1.0, stats.cost_result_cleanup_policy_scale * cost_model_scale_for_effect(stats, "device_host_io"));
        stats.source_best_effort_cleanup_gap_scale =
            std::min(stats.cost_source_best_effort_cleanup_scale, cleanup_io_scale);
    }
}

void collect_window_cost_intervals(const DagPatchStats & stats, const core::DagGraph & graph,
                                   const std::unordered_map<std::string, CpuIntervalLaneIndex> & lanes, size_t begin_node_id, size_t end_node_id,
                                   const std::string & kind, std::unordered_map<size_t, IntervalAttribution> & intervals_by_node) {
    if (begin_node_id >= graph.node_count() || end_node_id >= graph.node_count()) return;
    const auto & begin_node = graph.node(begin_node_id);
    const auto & end_node = graph.node(end_node_id);
    const auto & begin_event = graph.event_for_node(begin_node_id);
    const auto & end_event = graph.event_for_node(end_node_id);
    if (end_event.ts <= begin_event.ts || begin_node.lane_key != end_node.lane_key) return;
    const auto lane_it = lanes.find(begin_node.lane_key);
    if (lane_it == lanes.end()) return;
    const auto & intervals = lane_it->second.intervals;
    const auto begin_it =
        std::lower_bound(intervals.begin(), intervals.end(), begin_event.ts, [](const auto & entry, uint64_t value) { return entry.src_ts < value; });
    const auto end_it =
        std::upper_bound(intervals.begin(), intervals.end(), end_event.ts, [](uint64_t value, const auto & entry) { return value < entry.src_ts; });
    for (auto it = begin_it; it != end_it; ++it) {
        auto effect = interval_effect_kind(graph, *it, kind);
        if (!interval_effect_allowed_for_window(stats, kind, effect)) {
            if (kind == "extend_to_commit_compute" && effect_shape_enabled(stats, kResultPressureEffect)) {
                effect = std::string{ kResultPressureEffect };
            }
            else {
                continue;
            }
        }
        effect = normalize_effect_kind(std::move(effect));
        auto & attribution = intervals_by_node[it->node_id];
        attribution.window_kinds.insert(kind);
        attribution.effect_kinds.insert(std::move(effect));
    }
}

void collect_window_source_wait_gap_intervals(const DagPatchStats & stats, const core::DagGraph & graph,
                                              const std::unordered_map<std::string, CpuIntervalLaneIndex> & lanes, size_t begin_node_id,
                                              size_t end_node_id, const std::string & kind,
                                              std::unordered_map<size_t, IntervalAttribution> & intervals_by_node) {
    if (begin_node_id >= graph.node_count() || end_node_id >= graph.node_count()) return;
    const auto & begin_node = graph.node(begin_node_id);
    const auto & end_node = graph.node(end_node_id);
    const auto & begin_event = graph.event_for_node(begin_node_id);
    const auto & end_event = graph.event_for_node(end_node_id);
    if (end_event.ts <= begin_event.ts || begin_node.lane_key != end_node.lane_key) return;
    const auto lane_it = lanes.find(begin_node.lane_key);
    if (lane_it == lanes.end()) return;
    const auto & intervals = lane_it->second.intervals;
    const auto begin_it =
        std::lower_bound(intervals.begin(), intervals.end(), begin_event.ts, [](const auto & entry, uint64_t value) { return entry.src_ts < value; });
    const auto end_it =
        std::upper_bound(intervals.begin(), intervals.end(), end_event.ts, [](uint64_t value, const auto & entry) { return value < entry.src_ts; });
    for (auto it = begin_it; it != end_it; ++it) {
        auto effect = normalize_effect_kind(interval_effect_kind(graph, *it, kind));
        if (!source_wait_gap_effect(stats, effect)) {
            if (kind == "extend_to_commit_compute" && source_wait_gap_effect(stats, kResultPressureEffect))
                effect = std::string{ kResultPressureEffect };
            else
                continue;
        }
        auto & attribution = intervals_by_node[it->node_id];
        attribution.window_kinds.insert(kind);
        attribution.effect_kinds.insert(std::move(effect));
    }
}

void collect_window_source_best_effort_cleanup_gap_intervals(const DagPatchStats & stats, const core::DagGraph & graph,
                                                            const std::unordered_map<std::string, CpuIntervalLaneIndex> & lanes,
                                                            size_t begin_node_id, size_t end_node_id, const std::string & kind,
                                                            std::unordered_map<size_t, IntervalAttribution> & intervals_by_node) {
    if (begin_node_id >= graph.node_count() || end_node_id >= graph.node_count()) return;
    const auto & begin_node = graph.node(begin_node_id);
    const auto & end_node = graph.node(end_node_id);
    const auto & begin_event = graph.event_for_node(begin_node_id);
    const auto & end_event = graph.event_for_node(end_node_id);
    if (end_event.ts <= begin_event.ts || begin_node.lane_key != end_node.lane_key) return;
    const auto lane_it = lanes.find(begin_node.lane_key);
    if (lane_it == lanes.end()) return;
    const auto & intervals = lane_it->second.intervals;
    const auto begin_it =
        std::lower_bound(intervals.begin(), intervals.end(), begin_event.ts, [](const auto & entry, uint64_t value) { return entry.src_ts < value; });
    const auto end_it =
        std::upper_bound(intervals.begin(), intervals.end(), end_event.ts, [](uint64_t value, const auto & entry) { return value < entry.src_ts; });
    for (auto it = begin_it; it != end_it; ++it) {
        auto effect = normalize_effect_kind(interval_effect_kind(graph, *it, kind));
        if (!source_best_effort_cleanup_effect(stats, effect)) {
            if (kind == "extend_to_commit_compute" && source_best_effort_cleanup_effect(stats, kResultPressureEffect))
                effect = std::string{ kResultPressureEffect };
            else
                continue;
        }
        auto & attribution = intervals_by_node[it->node_id];
        attribution.window_kinds.insert(kind);
        attribution.effect_kinds.insert(std::move(effect));
    }
}

std::unordered_map<size_t, IntervalAttribution> collect_request_cost_intervals(const core::DagGraph & graph,
                                                                               const std::unordered_map<std::string, CpuIntervalLaneIndex> & lanes,
                                                                               const DagPatchStats & stats,
                                                                               std::unordered_map<std::string, RequestAnchors> & anchors_by_request) {
    std::unordered_map<size_t, IntervalAttribution> intervals_by_node;
    for (auto & [_, anchors] : anchors_by_request) {
        normalize_request_anchors(graph, anchors);
        for (const auto prefetch_node : anchors.prefetch_nodes) {
            const auto extend_node = first_node_at_or_after(graph, anchors.extend_nodes, graph.event_for_node(prefetch_node).ts);
            collect_window_cost_intervals(stats, graph, lanes, prefetch_node, extend_node, "prefetch_to_extend", intervals_by_node);
        }
        for (const auto lookup_node : anchors.lookup_nodes) {
            const auto extend_node = first_node_at_or_after(graph, anchors.extend_nodes, graph.event_for_node(lookup_node).ts);
            collect_window_cost_intervals(stats, graph, lanes, lookup_node, extend_node, "lookup_to_extend", intervals_by_node);
        }
        for (const auto extend_node : anchors.extend_nodes) {
            const auto commit_node = first_node_at_or_after(graph, anchors.commit_nodes, graph.event_for_node(extend_node).ts);
            const auto window_kind = anchors.result_io_blocking ? "extend_to_commit" : "extend_to_commit_compute";
            collect_window_cost_intervals(stats, graph, lanes, extend_node, commit_node, window_kind, intervals_by_node);
        }
    }
    return intervals_by_node;
}

std::unordered_map<size_t, IntervalAttribution> collect_source_wait_gap_intervals(
    const core::DagGraph & graph,
    const std::unordered_map<std::string, CpuIntervalLaneIndex> & lanes,
    const DagPatchStats & stats,
    std::unordered_map<std::string, RequestAnchors> & anchors_by_request) {
    std::unordered_map<size_t, IntervalAttribution> intervals_by_node;
    if (!source_wait_residue_relief_enabled(stats)) return intervals_by_node;
    for (auto & [_, anchors] : anchors_by_request) {
        normalize_request_anchors(graph, anchors);
        for (const auto extend_node : anchors.extend_nodes) {
            const auto commit_node = first_node_at_or_after(graph, anchors.commit_nodes, graph.event_for_node(extend_node).ts);
            collect_window_source_wait_gap_intervals(stats, graph, lanes, extend_node, commit_node, "extend_to_commit", intervals_by_node);
            collect_window_source_wait_gap_intervals(stats, graph, lanes, extend_node, commit_node, "extend_to_commit_compute", intervals_by_node);
        }
    }
    return intervals_by_node;
}

std::unordered_map<size_t, IntervalAttribution> collect_source_best_effort_cleanup_gap_intervals(
    const core::DagGraph & graph,
    const std::unordered_map<std::string, CpuIntervalLaneIndex> & lanes,
    const DagPatchStats & stats,
    std::unordered_map<std::string, RequestAnchors> & anchors_by_request) {
    std::unordered_map<size_t, IntervalAttribution> intervals_by_node;
    if (!source_best_effort_cleanup_relief_enabled(stats)) return intervals_by_node;
    for (auto & [_, anchors] : anchors_by_request) {
        normalize_request_anchors(graph, anchors);
        for (const auto extend_node : anchors.extend_nodes) {
            const auto commit_node = first_node_at_or_after(graph, anchors.commit_nodes, graph.event_for_node(extend_node).ts);
            collect_window_source_best_effort_cleanup_gap_intervals(
                stats, graph, lanes, extend_node, commit_node, "extend_to_commit", intervals_by_node);
            collect_window_source_best_effort_cleanup_gap_intervals(
                stats, graph, lanes, extend_node, commit_node, "extend_to_commit_compute", intervals_by_node);
        }
    }
    return intervals_by_node;
}

void record_scoped_cost_interval_stats(DagPatchStats & stats, core::DagGraph & graph,
                                       const std::unordered_map<size_t, IntervalAttribution> & intervals_by_node,
                                       const model::HiCacheSummary * source_summary) {
    stats.cost_interval_attribution_model = std::string{ kIntervalAttributionModel };
    (void)source_summary;
    if (intervals_by_node.empty()) {
        stats.warnings.push_back("semantic_patch_cost_interval_empty");
        return;
    }

    for (const auto & [node_id, attribution] : intervals_by_node) {
        if (node_id >= graph.node_count()) continue;
        const auto old_interval = read_u64_attr(graph.node(node_id), "cpuinterval", 0);
        if (old_interval == 0 || old_interval > kMaxCpuIntervalNs) continue;
        const auto new_interval = old_interval;

        ++stats.cost_interval_node_count;
        stats.cost_source_interval_ns += old_interval;
        stats.cost_mutated_interval_ns += new_interval;
        stats.cost_interval_delta_ns += static_cast<int64_t>(new_interval) - static_cast<int64_t>(old_interval);
        for (const auto & kind : attribution.window_kinds) {
            stats.cost_source_interval_by_kind[kind] += old_interval;
            stats.cost_mutated_interval_by_kind[kind] += new_interval;
        }
        if (attribution.effect_kinds.empty()) {
            ++stats.cost_unattributed_interval_node_count;
            stats.cost_unattributed_interval_ns += old_interval;
            continue;
        }
        for (const auto & effect : attribution.effect_kinds) {
            const auto impact = impact_category_for_effect(effect);
            stats.cost_source_interval_by_effect[effect] += old_interval;
            stats.cost_mutated_interval_by_effect[effect] += new_interval;
            stats.cost_interval_node_count_by_effect[effect]++;
            stats.cost_source_interval_by_impact[impact] += old_interval;
            stats.cost_mutated_interval_by_impact[impact] += new_interval;
            stats.cost_interval_node_count_by_impact[impact]++;
        }
    }
    if (stats.cost_interval_node_count > 0) stats.warnings.push_back("semantic_patch_cpuinterval_cost_mutation_disabled");
    if (stats.cost_source_interval_ns > 0) {
        stats.cost_interval_scale = static_cast<double>(stats.cost_mutated_interval_ns) / static_cast<double>(stats.cost_source_interval_ns);
    }
    for (const auto & [effect, source_ns] : stats.cost_source_interval_by_effect) {
        if (source_ns == 0) continue;
        const auto mutated_it = stats.cost_mutated_interval_by_effect.find(effect);
        const auto mutated_ns = mutated_it == stats.cost_mutated_interval_by_effect.end() ? uint64_t{ 0 } : mutated_it->second;
        stats.cost_interval_scale_by_effect[effect] = static_cast<double>(mutated_ns) / static_cast<double>(source_ns);
    }
    if (stats.cost_interval_node_count == 0) stats.warnings.push_back("semantic_patch_cost_interval_stat_empty");
}

void apply_source_wait_gap_patch(DagPatchStats & stats, core::DagGraph & graph,
                                 const std::unordered_map<size_t, IntervalAttribution> & intervals_by_node) {
    if (!source_wait_residue_relief_enabled(stats)) return;
    for (const auto & [node_id, attribution] : intervals_by_node) {
        if (node_id >= graph.node_count()) continue;
        const auto scale = source_wait_gap_interval_scale(stats, attribution);
        if (!scale) continue;
        const auto old_interval = read_u64_attr(graph.node(node_id), "cpuinterval", 0);
        if (old_interval == 0 || old_interval > kMaxCpuIntervalNs) continue;
        auto new_interval = static_cast<uint64_t>(std::llround(static_cast<double>(old_interval) * *scale));
        if (new_interval > kMaxCpuIntervalNs) new_interval = kMaxCpuIntervalNs;

        ++stats.source_wait_gap_interval_node_count;
        stats.source_wait_gap_source_interval_ns += old_interval;
        stats.source_wait_gap_mutated_interval_ns += new_interval;
        stats.source_wait_gap_interval_delta_ns += static_cast<int64_t>(new_interval) - static_cast<int64_t>(old_interval);
        if (new_interval == old_interval) continue;

        graph.set_node_attr(node_id, "cpuinterval", std::to_string(new_interval));
        ++stats.source_wait_gap_mutation_count;
        ++stats.mutation_count;
    }
    if (stats.source_wait_gap_interval_node_count == 0) stats.warnings.push_back("semantic_patch_source_wait_gap_empty");
}

void apply_source_best_effort_cleanup_gap_patch(DagPatchStats & stats, core::DagGraph & graph,
                                                const std::unordered_map<size_t, IntervalAttribution> & intervals_by_node) {
    if (!source_best_effort_cleanup_relief_enabled(stats)) return;
    for (const auto & [node_id, attribution] : intervals_by_node) {
        if (node_id >= graph.node_count()) continue;
        const auto scale = source_best_effort_cleanup_gap_interval_scale(stats, attribution);
        if (!scale) continue;
        const auto old_interval = read_u64_attr(graph.node(node_id), "cpuinterval", 0);
        if (old_interval == 0 || old_interval > kMaxCpuIntervalNs) continue;
        auto new_interval = static_cast<uint64_t>(std::llround(static_cast<double>(old_interval) * *scale));
        if (new_interval > kMaxCpuIntervalNs) new_interval = kMaxCpuIntervalNs;

        ++stats.source_best_effort_cleanup_gap_interval_node_count;
        stats.source_best_effort_cleanup_gap_source_interval_ns += old_interval;
        stats.source_best_effort_cleanup_gap_mutated_interval_ns += new_interval;
        stats.source_best_effort_cleanup_gap_interval_delta_ns += static_cast<int64_t>(new_interval) - static_cast<int64_t>(old_interval);
        if (new_interval == old_interval) continue;

        graph.set_node_attr(node_id, "cpuinterval", std::to_string(new_interval));
        ++stats.source_best_effort_cleanup_gap_mutation_count;
        ++stats.mutation_count;
    }
    if (stats.source_best_effort_cleanup_gap_interval_node_count == 0) stats.warnings.push_back("semantic_patch_source_best_effort_cleanup_gap_empty");
}

std::unordered_map<std::string, RequestAnchors> build_request_anchors_from_summary(const core::DagGraph & graph, const model::HiCacheSummary & summary) {
    std::unordered_map<std::string, RequestAnchors> anchors_by_request;
    for (const auto & decision : summary.policy_decision_trace) {
        if (decision.request_key.empty() || decision.source_node_id >= graph.node_count()) continue;
        add_anchor(anchors_by_request[decision.request_key], decision.role, decision.source_node_id);
    }
    for (const auto & transition : summary.transition_trace) {
        const auto request_key = transition_request_key(transition);
        if (!request_key.empty()) note_transition_effect(anchors_by_request[request_key], transition);
        if (transition.source_node_id >= graph.node_count()) continue;
        if (!request_key.empty()) add_anchor(anchors_by_request[request_key], transition.role, transition.source_node_id);
    }
    return anchors_by_request;
}

std::vector<CriticalPredecessor> critical_predecessors_by_node(const core::DagGraph & graph) {
    constexpr size_t invalid = std::numeric_limits<size_t>::max();
    std::vector<CriticalPredecessor> best_pred_by_node(graph.node_count());
    for (const auto & edge : graph.edges()) {
        if (edge.src >= graph.node_count() || edge.dst >= graph.node_count()) continue;
        const auto & pred_node = graph.node(edge.src);
        auto & current_pred = best_pred_by_node[edge.dst];
        if (current_pred.node_id == invalid || pred_node.completion_time > graph.node(current_pred.node_id).completion_time)
            current_pred = CriticalPredecessor{ .node_id = edge.src, .edge_kind = edge.kind };
    }
    return best_pred_by_node;
}

size_t critical_path_terminal_node(const core::DagGraph & graph) {
    size_t current = 0;
    for (const auto & node : graph.nodes()) {
        if (node.completion_time > graph.node(current).completion_time) current = node.id;
    }
    return current;
}

void record_critical_path_hicache_attribution(core::DagGraph & graph, model::HiCacheSummary & summary) {
    constexpr size_t invalid = std::numeric_limits<size_t>::max();
    if (!summary.target_config.enable_dag_patch || graph.node_count() == 0) return;
    auto anchors_by_request = build_request_anchors_from_summary(graph, summary);
    auto interval_lanes = build_cpu_interval_lane_index(graph);
    DagPatchStats stats;
    record_cost_work_units(summary, nullptr, stats);
    auto hicache_intervals = collect_request_cost_intervals(graph, interval_lanes, stats, anchors_by_request);
    if (hicache_intervals.empty()) {
        summary.dag_patch_warnings.push_back("semantic_patch_critical_path_hicache_interval_empty");
        return;
    }

    const auto best_pred_by_node = critical_predecessors_by_node(graph);
    std::unordered_set<size_t> seen;
    auto current = critical_path_terminal_node(graph);
    while (current != invalid && current < graph.node_count() && seen.insert(current).second) {
        const auto pred_ref = best_pred_by_node[current];
        const auto pred = pred_ref.node_id;
        if (pred == invalid || pred >= graph.node_count()) break;
        if (pred_ref.edge_kind == core::DagEdgeKind::HiCache) {
            ++summary.dag_patch_critical_path_hicache_edge_count;
            const auto edge_kind = graph.node_attr(current, shape_edge_attr_key(pred), "unknown");
            summary.dag_patch_critical_path_hicache_edges_by_kind[edge_kind]++;
            summary.dag_patch_critical_path_hicache_edges_by_impact[shape_edge_impact_category(edge_kind)]++;
        }
        const auto interval = read_u64_attr(graph.node(pred), "cpuinterval", 0);
        if (interval > 0 && interval <= kMaxCpuIntervalNs) {
            ++summary.dag_patch_critical_path_interval_node_count;
            summary.dag_patch_critical_path_interval_ns += interval;
            const auto attribution_it = hicache_intervals.find(pred);
            if (attribution_it != hicache_intervals.end()) {
                ++summary.dag_patch_critical_path_hicache_interval_node_count;
                summary.dag_patch_critical_path_hicache_interval_ns += interval;
                for (const auto & kind : attribution_it->second.window_kinds) { summary.dag_patch_critical_path_hicache_interval_by_kind[kind] += interval; }
                for (const auto & effect : attribution_it->second.effect_kinds) {
                    summary.dag_patch_critical_path_hicache_interval_by_effect[effect] += interval;
                    summary.dag_patch_critical_path_hicache_interval_by_impact[impact_category_for_effect(effect)] += interval;
                }
            }
        }
        current = pred;
    }
}

DagPatchStats apply_semantic_dag_shape_patch(core::DagGraph & graph, const frontend::HiCacheConfig & config, const model::HiCacheSummary & summary,
                                             const model::HiCacheSummary * source_summary) {
    DagPatchStats stats;
    if (!config.enable_dag_patch || graph.node_count() == 0) return stats;

    std::unordered_set<size_t> scoped_nodes;
    std::unordered_map<size_t, std::unordered_set<std::string>> roles_by_node;
    std::unordered_map<std::string, RequestAnchors> anchors_by_request;

    for (const auto & decision : summary.policy_decision_trace) {
        ++stats.policy_decision_count;
        stats.intent_by_policy_area[decision.policy_area.empty() ? "unknown" : decision.policy_area]++;
        stats.intent_by_role[decision.role.empty() ? "unknown" : decision.role]++;
        if (decision.source_node_id < graph.node_count()) {
            scoped_nodes.insert(decision.source_node_id);
            add_role(roles_by_node, decision.source_node_id, decision.role);
        }
        if (!decision.request_key.empty() && decision.source_node_id < graph.node_count())
            add_anchor(anchors_by_request[decision.request_key], decision.role, decision.source_node_id);
    }

    for (const auto & transition : summary.transition_trace) {
        ++stats.transition_count;
        stats.intent_by_role[transition.role.empty() ? "unknown" : transition.role]++;
        const auto request_key = transition_request_key(transition);
        if (!request_key.empty()) note_transition_effect(anchors_by_request[request_key], transition);
        if (transition.source_node_id >= graph.node_count()) continue;
        scoped_nodes.insert(transition.source_node_id);
        add_role(roles_by_node, transition.source_node_id, transition.role);
        if (!request_key.empty()) add_anchor(anchors_by_request[request_key], transition.role, transition.source_node_id);
    }

    stats.scoped_node_count = scoped_nodes.size();
    std::unordered_map<size_t, std::unordered_set<std::string>> interval_roles_by_node;
    for (const auto & edge : graph.edges()) {
        if (edge.kind != core::DagEdgeKind::Sequential || edge.src >= graph.node_count() || edge.dst >= graph.node_count()) continue;
        const auto src_scoped = scoped_nodes.contains(edge.src);
        const auto dst_scoped = scoped_nodes.contains(edge.dst);
        if (!src_scoped && !dst_scoped) continue;
        if (src_scoped) merge_roles(interval_roles_by_node, edge.src, roles_by_node[edge.src]);
        if (dst_scoped) merge_roles(interval_roles_by_node, edge.src, roles_by_node[edge.dst]);
    }

    for (const auto & [node_id, roles] : interval_roles_by_node) {
        const auto interval = read_u64_attr(graph.node(node_id), "cpuinterval", 0);
        if (interval == 0 || interval > kMaxCpuIntervalNs) continue;
        ++stats.scoped_interval_node_count;
        stats.scoped_interval_ns += interval;
        for (const auto & role : roles) { stats.scoped_interval_by_role[role.empty() ? "unknown" : role] += interval; }
    }

    if (stats.scoped_node_count == 0) stats.warnings.push_back("semantic_patch_scope_empty");
    if (stats.scoped_interval_node_count == 0) stats.warnings.push_back("semantic_patch_scoped_cpuinterval_empty");
    record_cost_work_units(summary, source_summary, stats);
    add_request_shape_edges(stats, graph, anchors_by_request);
    const auto physical_nodes = build_physical_node_index(graph);
    add_request_physical_shape_edges(stats, graph, physical_nodes, anchors_by_request);
    if (stats.physical_window_count == 0) stats.warnings.push_back("semantic_patch_physical_window_empty");
    if (stats.physical_node_count == 0) stats.warnings.push_back("semantic_patch_physical_node_empty");
    if (stats.physical_shape_edge_count == 0) stats.warnings.push_back("semantic_patch_physical_shape_edge_empty");
    mutate_request_physical_durations(stats, graph, physical_nodes, anchors_by_request);
    auto interval_lanes = build_cpu_interval_lane_index(graph);
    add_request_window_stats(stats, graph, interval_lanes, anchors_by_request);
    add_target_wait_carrier(stats, graph, interval_lanes, anchors_by_request, summary, source_summary);
    if (stats.shape_edge_count == 0) stats.warnings.push_back("semantic_patch_shape_edge_empty");
    if (stats.window_count == 0) stats.warnings.push_back("semantic_patch_request_window_empty");
    if (stats.window_interval_node_count == 0) stats.warnings.push_back("semantic_patch_request_window_cpuinterval_empty");
    auto cost_intervals = collect_request_cost_intervals(graph, interval_lanes, stats, anchors_by_request);
    record_scoped_cost_interval_stats(stats, graph, cost_intervals, source_summary);
    auto source_wait_gap_intervals = collect_source_wait_gap_intervals(graph, interval_lanes, stats, anchors_by_request);
    apply_source_wait_gap_patch(stats, graph, source_wait_gap_intervals);
    auto source_best_effort_cleanup_gap_intervals =
        collect_source_best_effort_cleanup_gap_intervals(graph, interval_lanes, stats, anchors_by_request);
    apply_source_best_effort_cleanup_gap_patch(stats, graph, source_best_effort_cleanup_gap_intervals);
    return stats;
}
#endif

} // namespace hicache_module_detail

#ifdef DEBUG
using hicache_module_detail::apply_semantic_dag_shape_patch;
#endif
using hicache_module_detail::kModuleName;

HiCacheModule::HiCacheModule(frontend::HiCacheConfig config) : config_(std::move(config)) {}

HiCacheModule::HiCacheModule(frontend::HiCacheConfig config, std::optional<frontend::HiCacheConfig> source_config)
    : config_(std::move(config)),
      source_config_(std::move(source_config)) {}

std::string HiCacheModule::name() const { return std::string{ kModuleName }; }

/**
 * @brief 将 HiCache state model 作为 SimulationModule 执行。
 *
 * 该模块先执行 source/target state replay，再在显式开启 cache patch 时把
 * target HiCache intent 投影成 source DAG 上的 scoped HiCache dependency edges，
 * 并在这些 request windows 内把 CPU interval 归因到 typed HiCache effects。真正
 * 修改 duration 之前必须有 effect-specific cost model，不做全窗口 scale。
 * 该路径不做全图 `cpuinterval` fitting，也不读取 target trace timing。
 */
void HiCacheModule::apply(core::DagGraph & graph) {
#ifdef DEBUG
    std::optional<model::HiCacheSummary> source_summary;
    if (config_.enable_dag_patch && source_config_ && source_config_->enabled) { source_summary = model::apply_hicache_model(graph, *source_config_); }
#endif
    auto summary = model::apply_hicache_model(graph, config_);
#ifdef DEBUG
    auto patch_stats = apply_semantic_dag_shape_patch(graph, config_, summary, source_summary ? &*source_summary : nullptr);
    summary.dag_mutations = patch_stats.mutation_count;
    summary.dag_patch_model = config_.enable_dag_patch ? std::string{ hicache_module_detail::kSemanticHiCacheShapePatchModel } : "";
    summary.dag_patch_policy_decision_count = patch_stats.policy_decision_count;
    summary.dag_patch_transition_count = patch_stats.transition_count;
    summary.dag_patch_scoped_node_count = patch_stats.scoped_node_count;
    summary.dag_patch_scoped_interval_node_count = patch_stats.scoped_interval_node_count;
    summary.dag_patch_scoped_interval_ns = patch_stats.scoped_interval_ns;
    summary.dag_patch_shape_edge_count = patch_stats.shape_edge_count;
    summary.dag_patch_skipped_reverse_shape_edge_count = patch_stats.skipped_reverse_shape_edge_count;
    summary.dag_patch_window_count = patch_stats.window_count;
    summary.dag_patch_window_interval_node_count = patch_stats.window_interval_node_count;
    summary.dag_patch_window_interval_ns = patch_stats.window_interval_ns;
    summary.dag_patch_physical_window_count = patch_stats.physical_window_count;
    summary.dag_patch_physical_node_count = patch_stats.physical_node_count;
    summary.dag_patch_physical_interval_node_count = patch_stats.physical_interval_node_count;
    summary.dag_patch_physical_interval_ns = patch_stats.physical_interval_ns;
    summary.dag_patch_physical_shape_edge_count = patch_stats.physical_shape_edge_count;
    summary.dag_patch_physical_duration_mutation_count = patch_stats.physical_duration_mutation_count;
    summary.dag_patch_physical_source_duration_ns = patch_stats.physical_source_duration_ns;
    summary.dag_patch_physical_mutated_duration_ns = patch_stats.physical_mutated_duration_ns;
    summary.dag_patch_physical_duration_delta_ns = patch_stats.physical_duration_delta_ns;
    summary.dag_patch_target_wait_carrier_count = patch_stats.target_wait_carrier_count;
    summary.dag_patch_target_wait_carrier_duration_ns = patch_stats.target_wait_carrier_duration_ns;
    summary.dag_patch_source_wait_gap_mutation_count = patch_stats.source_wait_gap_mutation_count;
    summary.dag_patch_source_wait_gap_interval_node_count = patch_stats.source_wait_gap_interval_node_count;
    summary.dag_patch_source_wait_gap_source_interval_ns = patch_stats.source_wait_gap_source_interval_ns;
    summary.dag_patch_source_wait_gap_mutated_interval_ns = patch_stats.source_wait_gap_mutated_interval_ns;
    summary.dag_patch_source_wait_gap_interval_delta_ns = patch_stats.source_wait_gap_interval_delta_ns;
    summary.dag_patch_source_wait_gap_base_scale = patch_stats.source_wait_gap_base_scale;
    summary.dag_patch_source_wait_gap_device_host_scale = patch_stats.source_wait_gap_device_host_scale;
    summary.dag_patch_source_best_effort_cleanup_gap_mutation_count = patch_stats.source_best_effort_cleanup_gap_mutation_count;
    summary.dag_patch_source_best_effort_cleanup_gap_interval_node_count = patch_stats.source_best_effort_cleanup_gap_interval_node_count;
    summary.dag_patch_source_best_effort_cleanup_gap_source_interval_ns = patch_stats.source_best_effort_cleanup_gap_source_interval_ns;
    summary.dag_patch_source_best_effort_cleanup_gap_mutated_interval_ns = patch_stats.source_best_effort_cleanup_gap_mutated_interval_ns;
    summary.dag_patch_source_best_effort_cleanup_gap_interval_delta_ns = patch_stats.source_best_effort_cleanup_gap_interval_delta_ns;
    summary.dag_patch_source_best_effort_cleanup_gap_scale = patch_stats.source_best_effort_cleanup_gap_scale;
    summary.dag_patch_cost_mutation_count = patch_stats.cost_mutation_count;
    summary.dag_patch_cost_interval_node_count = patch_stats.cost_interval_node_count;
    summary.dag_patch_cost_source_interval_ns = patch_stats.cost_source_interval_ns;
    summary.dag_patch_cost_mutated_interval_ns = patch_stats.cost_mutated_interval_ns;
    summary.dag_patch_cost_interval_delta_ns = patch_stats.cost_interval_delta_ns;
    summary.dag_patch_cost_interval_scale = patch_stats.cost_interval_scale;
    summary.dag_patch_cost_unattributed_interval_node_count = patch_stats.cost_unattributed_interval_node_count;
    summary.dag_patch_cost_unattributed_interval_ns = patch_stats.cost_unattributed_interval_ns;
    summary.dag_patch_cost_source_work_units = patch_stats.cost_source_work_units;
    summary.dag_patch_cost_target_work_units = patch_stats.cost_target_work_units;
    summary.dag_patch_cost_source_intent_units = patch_stats.cost_source_intent_units;
    summary.dag_patch_cost_target_intent_units = patch_stats.cost_target_intent_units;
    summary.dag_patch_cost_source_blocking_result_score = patch_stats.cost_source_blocking_result_score;
    summary.dag_patch_cost_target_blocking_result_score = patch_stats.cost_target_blocking_result_score;
    summary.dag_patch_cost_source_wait_score = patch_stats.cost_source_wait_score;
    summary.dag_patch_cost_target_wait_score = patch_stats.cost_target_wait_score;
    summary.dag_patch_cost_source_wait_residue_scale = patch_stats.cost_source_wait_residue_scale;
    summary.dag_patch_cost_target_wait_carrier_score = patch_stats.cost_target_wait_carrier_score;
    summary.dag_patch_cost_source_best_effort_cleanup_scale = patch_stats.cost_source_best_effort_cleanup_scale;
    summary.dag_patch_cost_result_cleanup_policy_scale = patch_stats.cost_result_cleanup_policy_scale;
    summary.dag_patch_cost_result_effect_scale = patch_stats.cost_result_effect_scale;
    summary.dag_patch_cost_prefill_compute_scale = patch_stats.cost_prefill_compute_scale;
    summary.dag_patch_cost_result_io_blocking_scale = patch_stats.cost_result_io_blocking_scale;
    summary.dag_patch_cost_work_unit_model = std::move(patch_stats.cost_work_unit_model);
    summary.dag_patch_cost_interval_attribution_model = std::move(patch_stats.cost_interval_attribution_model);
    summary.dag_patch_intent_by_policy_area = std::move(patch_stats.intent_by_policy_area);
    summary.dag_patch_intent_by_role = std::move(patch_stats.intent_by_role);
    summary.dag_patch_shape_edges_by_kind = std::move(patch_stats.shape_edges_by_kind);
    summary.dag_patch_shape_edges_by_impact = std::move(patch_stats.shape_edges_by_impact);
    summary.dag_patch_scoped_interval_by_role = std::move(patch_stats.scoped_interval_by_role);
    summary.dag_patch_window_interval_by_kind = std::move(patch_stats.window_interval_by_kind);
    summary.dag_patch_physical_nodes_by_kind = std::move(patch_stats.physical_nodes_by_kind);
    summary.dag_patch_physical_nodes_by_impact = std::move(patch_stats.physical_nodes_by_impact);
    summary.dag_patch_physical_interval_by_kind = std::move(patch_stats.physical_interval_by_kind);
    summary.dag_patch_physical_interval_by_impact = std::move(patch_stats.physical_interval_by_impact);
    summary.dag_patch_physical_shape_edges_by_kind = std::move(patch_stats.physical_shape_edges_by_kind);
    summary.dag_patch_physical_shape_edges_by_impact = std::move(patch_stats.physical_shape_edges_by_impact);
    summary.dag_patch_physical_source_duration_by_effect = std::move(patch_stats.physical_source_duration_by_effect);
    summary.dag_patch_physical_mutated_duration_by_effect = std::move(patch_stats.physical_mutated_duration_by_effect);
    summary.dag_patch_physical_duration_node_count_by_effect = std::move(patch_stats.physical_duration_node_count_by_effect);
    summary.dag_patch_physical_duration_scale_by_effect = std::move(patch_stats.physical_duration_scale_by_effect);
    summary.dag_patch_cost_source_interval_by_kind = std::move(patch_stats.cost_source_interval_by_kind);
    summary.dag_patch_cost_mutated_interval_by_kind = std::move(patch_stats.cost_mutated_interval_by_kind);
    summary.dag_patch_cost_source_interval_by_effect = std::move(patch_stats.cost_source_interval_by_effect);
    summary.dag_patch_cost_mutated_interval_by_effect = std::move(patch_stats.cost_mutated_interval_by_effect);
    summary.dag_patch_cost_source_interval_by_impact = std::move(patch_stats.cost_source_interval_by_impact);
    summary.dag_patch_cost_mutated_interval_by_impact = std::move(patch_stats.cost_mutated_interval_by_impact);
    summary.dag_patch_cost_interval_scale_by_effect = std::move(patch_stats.cost_interval_scale_by_effect);
    summary.dag_patch_cost_interval_node_count_by_effect = std::move(patch_stats.cost_interval_node_count_by_effect);
    summary.dag_patch_cost_interval_node_count_by_impact = std::move(patch_stats.cost_interval_node_count_by_impact);
    summary.dag_patch_cost_source_work_units_by_effect = std::move(patch_stats.cost_source_work_units_by_effect);
    summary.dag_patch_cost_target_work_units_by_effect = std::move(patch_stats.cost_target_work_units_by_effect);
    summary.dag_patch_cost_source_work_units_by_impact = std::move(patch_stats.cost_source_work_units_by_impact);
    summary.dag_patch_cost_target_work_units_by_impact = std::move(patch_stats.cost_target_work_units_by_impact);
    summary.dag_patch_cost_source_work_units_by_effect_policy_area = std::move(patch_stats.cost_source_work_units_by_effect_policy_area);
    summary.dag_patch_cost_target_work_units_by_effect_policy_area = std::move(patch_stats.cost_target_work_units_by_effect_policy_area);
    summary.dag_patch_cost_source_work_units_by_impact_policy_area = std::move(patch_stats.cost_source_work_units_by_impact_policy_area);
    summary.dag_patch_cost_target_work_units_by_impact_policy_area = std::move(patch_stats.cost_target_work_units_by_impact_policy_area);
    summary.dag_patch_cost_source_intent_units_by_effect_policy_area = std::move(patch_stats.cost_source_intent_units_by_effect_policy_area);
    summary.dag_patch_cost_target_intent_units_by_effect_policy_area = std::move(patch_stats.cost_target_intent_units_by_effect_policy_area);
    summary.dag_patch_cost_source_intent_units_by_impact_policy_area = std::move(patch_stats.cost_source_intent_units_by_impact_policy_area);
    summary.dag_patch_cost_target_intent_units_by_impact_policy_area = std::move(patch_stats.cost_target_intent_units_by_impact_policy_area);
    summary.dag_patch_cost_source_work_units_by_policy_area = std::move(patch_stats.cost_source_work_units_by_policy_area);
    summary.dag_patch_cost_target_work_units_by_policy_area = std::move(patch_stats.cost_target_work_units_by_policy_area);
    summary.dag_patch_cost_source_work_units_by_role = std::move(patch_stats.cost_source_work_units_by_role);
    summary.dag_patch_cost_target_work_units_by_role = std::move(patch_stats.cost_target_work_units_by_role);
    summary.dag_patch_cost_model_scale_by_effect = std::move(patch_stats.cost_model_scale_by_effect);
    summary.dag_patch_warnings = std::move(patch_stats.warnings);
    summary_ = std::move(summary);
    applied_ = true;
#else
    (void)summary;
#endif
}

void HiCacheModule::after_simulation(core::DagGraph & graph) {
#ifdef DEBUG
    if (!applied_) return;
    hicache_module_detail::record_critical_path_hicache_attribution(graph, summary_);
#else
    (void)graph;
#endif
}

bool HiCacheModule::has_summary() const {
#ifdef DEBUG
    return applied_;
#else
    return false;
#endif
}

} // namespace markov::trace_graph::modules::hicache
