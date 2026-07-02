/**
 * @file
 * @brief 阶段一 DAG analysis JSON artifact 构造实现。
 */
#include "markov/trace_graph/modules/dag_analysis/diagnostics.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <map>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace markov::trace_graph::modules::dag_analysis {

namespace dag_analysis_detail {

using Json = nlohmann::json;

constexpr double kFaithfulReplayRelErrorMax = 0.05;
constexpr size_t kCriticalPathSampleLimit = 50;
constexpr uint64_t kNearbyAnchorWindowNs = 1'000'000;
constexpr size_t kSampleLimit = 8;

struct FactInfo {
    bool present = false;
    std::string fact_class;
    std::string role;
};

struct RoleAnchorStats {
    size_t fact_count = 0;
    size_t direct_fact_node_count = 0;
    size_t near_runtime_anchor_count = 0;
    size_t timestamp_only_anchor_count = 0;
    size_t missing_anchor_count = 0;
    Json sample_missing = Json::array();
};

struct OperationEvidence {
    std::string name;
    std::vector<size_t> direct_runtime_nodes;
    std::vector<size_t> control_fact_nodes;
    std::vector<size_t> state_fact_nodes;
};

size_t sum_role_value(const std::map<std::string, RoleAnchorStats> & stats, size_t RoleAnchorStats::*member);

std::string edge_kind_name(core::DagEdgeKind kind) {
    switch (kind) {
    case core::DagEdgeKind::Sequential:
        return "sequential";
    case core::DagEdgeKind::Stream:
        return "stream";
    case core::DagEdgeKind::Correlation:
        return "correlation";
    case core::DagEdgeKind::Sync:
        return "sync";
    case core::DagEdgeKind::HCCL:
        return "hccl";
    case core::DagEdgeKind::HiCache:
        return "hicache";
    case core::DagEdgeKind::Mutation:
        return "mutation";
    }
    return "unknown";
}

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool contains(std::string_view haystack, std::string_view needle) { return haystack.find(needle) != std::string_view::npos; }

std::string event_domain(const core::TraceEvent & event) {
    auto domain = event.arg("domain");
    if (!domain.empty()) return domain;
    if (!event.cat.empty()) return event.cat;
    return "unknown";
}

std::string event_source(const core::TraceEvent & event) {
    auto source = event.arg("trace_source");
    if (!source.empty()) return source;
    source = event.arg("source");
    if (!source.empty()) return source;
    auto domain = event_domain(event);
    if (domain == "python_probe") return "python_probe";
    if (event.name.starts_with("AscendCL@") || event.name == "EVENT_RECORD" || event.name == "EVENT_WAIT") return "ld_preload";
    if (event.has_arg("Physic Stream Id") || event.cat == "Kernel") return "torch_or_device";
    return domain;
}

bool is_python_probe_fact_node(const core::TraceEvent & event) {
    return event.arg("domain") == "python_probe" || event.cat == "hicache" || event.name.starts_with("hicache_");
}

bool is_runtime_node(const core::TraceEvent & event) { return !is_python_probe_fact_node(event); }

Json parse_fact_json(const std::string & raw) {
    if (raw.empty()) return Json{};
    auto value = Json::parse(raw);
    if (value.is_string()) value = Json::parse(value.get<std::string>());
    return value;
}

FactInfo parse_fact(const core::TraceEvent & event) {
    FactInfo info;
    if (!event.has_arg("fact")) return info;
    try {
        auto fact = parse_fact_json(event.arg("fact"));
        if (!fact.is_object()) return info;
        info.fact_class = fact.value("class", "");
        info.role = fact.value("role", "");
        info.present = !info.fact_class.empty() && !info.role.empty();
    }
    catch (...) {
        info.present = false;
    }
    return info;
}

Json compact_node_json(const core::DagGraph & graph, size_t node_id) {
    const auto & node = graph.node(node_id);
    const auto & event = graph.event_for_node(node_id);
    return Json{
        {             "node_id",               node.id },
        {         "event_index",      node.event_index },
        {                "name",            event.name },
        {                 "cat",             event.cat },
        {              "domain",   event_domain(event) },
        {                "lane",         node.lane_key },
        {              "is_cpu",           node.is_cpu },
        {         "duration_ns",         node.duration },
        { "simulation_start_ns", node.simulation_start },
        {       "completion_ns",  node.completion_time },
    };
}

void increment(Json & object, const std::string & key) { object[key] = object.value(key, 0) + 1; }

Json lane_summary(const core::DagGraph & graph) {
    std::map<std::string, Json> cpu;
    std::map<std::string, Json> device;
    std::map<std::string, Json> hccl;
    for (const auto & node : graph.nodes()) {
        auto & lanes = node.is_cpu ? cpu : device;
        auto & item = lanes[node.lane_key];
        if (!item.is_object()) item = Json::object();
        item["node_count"] = item.value("node_count", 0) + 1;
        item["duration_ns"] = item.value("duration_ns", uint64_t{ 0 }) + node.duration;
        const auto & event = graph.event_for_node(node.id);
        if (!node.is_cpu && (event.name.contains("hcom") || event.name.contains("HCCL") || event.name.contains("hccl"))) {
            auto & hccl_item = hccl[node.lane_key];
            if (!hccl_item.is_object()) hccl_item = Json::object();
            hccl_item["node_count"] = hccl_item.value("node_count", 0) + 1;
            hccl_item["duration_ns"] = hccl_item.value("duration_ns", uint64_t{ 0 }) + node.duration;
        }
    }
    return Json{
        {    "cpu",    cpu },
        { "device", device },
        {   "hccl",   hccl }
    };
}

Json critical_path_sample(const core::DagGraph & graph) {
    constexpr size_t invalid = std::numeric_limits<size_t>::max();
    if (graph.node_count() == 0)
        return Json{
            { "total_duration_ns", graph.e2e_time() },
            {      "sample_nodes",    Json::array() }
        };

    std::vector<std::vector<std::pair<size_t, core::DagEdgeKind>>> incoming(graph.node_count());
    for (const auto & edge : graph.edges()) {
        if (edge.src < graph.node_count() && edge.dst < graph.node_count()) incoming[edge.dst].push_back({ edge.src, edge.kind });
    }

    size_t current = 0;
    for (const auto & node : graph.nodes()) {
        if (node.completion_time > graph.node(current).completion_time) current = node.id;
    }

    std::vector<Json> reversed;
    std::unordered_set<size_t> seen;
    std::string incoming_edge_kind;
    while (current != invalid && reversed.size() < kCriticalPathSampleLimit && seen.insert(current).second) {
        auto item = compact_node_json(graph, current);
        if (!incoming_edge_kind.empty()) item["incoming_edge_kind"] = incoming_edge_kind;
        reversed.push_back(std::move(item));

        size_t best_pred = invalid;
        core::DagEdgeKind best_kind = core::DagEdgeKind::Sequential;
        uint64_t best_completion = 0;
        for (const auto & pred : incoming[current]) {
            const auto & pred_node = graph.node(pred.first);
            if (best_pred == invalid || pred_node.completion_time > best_completion) {
                best_pred = pred.first;
                best_kind = pred.second;
                best_completion = pred_node.completion_time;
            }
        }
        if (best_pred == invalid) break;
        incoming_edge_kind = edge_kind_name(best_kind);
        current = best_pred;
    }
    std::ranges::reverse(reversed);
    return Json{
        { "total_duration_ns", graph.e2e_time() },
        {      "sample_nodes",         reversed },
    };
}

Json build_dag_quality(const core::DagGraph & graph) {
    const auto real = graph.real_e2e_time();
    const auto simulated = graph.e2e_time();
    const auto abs_error = real > simulated ? real - simulated : simulated - real;
    Json rel_error = nullptr;
    if (real > 0) rel_error = static_cast<double>(abs_error) / static_cast<double>(real);
    Json blockers = Json::array();
    if (real == 0) blockers.push_back("missing_trace_real_e2e");
    if (real > 0 && static_cast<double>(abs_error) / static_cast<double>(real) > kFaithfulReplayRelErrorMax) blockers.push_back("dag_replay_error_too_high");

    std::unordered_set<std::string> cpu_lanes;
    std::unordered_set<std::string> device_lanes;
    for (const auto & node : graph.nodes()) {
        if (node.is_cpu) cpu_lanes.insert(node.lane_key);
        else device_lanes.insert(node.lane_key);
    }

    return Json{
        {                 "schema","markov.trace_graph.dag_quality.v1"                                   },
        {                    "run",                      Json::object() },
        { "trace_channel_coverage",                      Json::object() },
        {              "dag_build",
         Json{
         { "parsed_record_count", graph.parsed_record_count() },
         { "normalized_event_count", graph.events().size() },
         { "node_count", graph.node_count() },
         { "edge_count", graph.edge_count() },
         { "edge_counts_by_kind", graph.edge_counts_by_kind() },
         { "cpu_lane_count", cpu_lanes.size() },
         { "device_lane_count", device_lanes.size() },
         }                                                             },
        {        "faithful_replay",
         Json{
         { "real_e2e_ns", real },
         { "simulated_e2e_ns", simulated },
         { "absolute_error_ns", abs_error },
         { "relative_error", rel_error },
         { "threshold_relative_error", kFaithfulReplayRelErrorMax },
         { "ready", blockers.empty() },
         { "blockers", blockers },
         }                                                             },
    };
}

Json build_dag_analysis(const core::DagGraph & graph) {
    Json by_source = Json::object();
    Json by_domain = Json::object();
    Json by_category = Json::object();
    const auto edge_counts = graph.edge_counts_by_kind();
    const auto sync_edges = edge_counts.contains("sync") ? edge_counts.at("sync") : 0;
    const auto correlation_edges = edge_counts.contains("correlation") ? edge_counts.at("correlation") : 0;
    Json sync_coverage = {
        {  "event_wait_nodes",          0 },
        { "stream_sync_nodes",          0 },
        {  "event_sync_nodes",          0 },
        { "device_sync_nodes",          0 },
        { "notify_wait_nodes",          0 },
        {        "sync_edges", sync_edges },
    };
    Json correlation_coverage = {
        { "correlation_groups",                 0 },
        {  "connection_groups",                 0 },
        {  "correlation_edges", correlation_edges },
    };
    std::unordered_set<std::string> correlation_groups;
    std::unordered_set<std::string> connection_groups;

    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        increment(by_source, event_source(event));
        increment(by_domain, event_domain(event));
        increment(by_category, event.cat.empty() ? "unknown" : event.cat);
        if (event.name == "EVENT_WAIT") sync_coverage["event_wait_nodes"] = sync_coverage.value("event_wait_nodes", 0) + 1;
        else if (event.name == "AscendCL@aclrtSynchronizeStream" || event.name == "AscendCL@aclrtSynchronizeStreamWithTimeout")
            sync_coverage["stream_sync_nodes"] = sync_coverage.value("stream_sync_nodes", 0) + 1;
        else if (event.name == "AscendCL@aclrtSynchronizeEvent" || event.name == "AscendCL@aclrtSynchronizeEventWithTimeout")
            sync_coverage["event_sync_nodes"] = sync_coverage.value("event_sync_nodes", 0) + 1;
        else if (event.name == "AscendCL@aclrtSynchronizeDevice" || event.name == "AscendCL@aclrtSynchronizeDeviceWithTimeout")
            sync_coverage["device_sync_nodes"] = sync_coverage.value("device_sync_nodes", 0) + 1;
        else if (event.name == "NOTIFY_WAIT") sync_coverage["notify_wait_nodes"] = sync_coverage.value("notify_wait_nodes", 0) + 1;
        if (event.has_arg("correlation_id")) correlation_groups.insert(event.arg("correlation_id"));
        if (event.has_arg("connection_id")) connection_groups.insert(event.arg("connection_id"));
    }
    correlation_coverage["correlation_groups"] = correlation_groups.size();
    correlation_coverage["connection_groups"] = connection_groups.size();
    Json blockers = Json::array();
    if (graph.node_count() == 0) blockers.push_back("empty_dag");
    if (graph.edge_count() == 0 && graph.node_count() > 1) blockers.push_back("dag_edge_coverage_gap");

    return Json{
        {                 "schema", "markov.trace_graph.dag_analysis.v1" },
        {   "node_count_by_source",                            by_source },
        {   "node_count_by_domain",                            by_domain },
        { "node_count_by_category",                          by_category },
        {     "edge_count_by_kind",                          edge_counts },
        {           "lane_summary",                  lane_summary(graph) },
        {          "sync_coverage",                        sync_coverage },
        {   "correlation_coverage",                 correlation_coverage },
        {          "critical_path",          critical_path_sample(graph) },
        {               "blockers",                             blockers },
    };
}

bool has_near_runtime_anchor(const core::DagGraph & graph, const core::TraceEvent & fact_event) {
    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        if (!is_runtime_node(event)) continue;
        auto delta = event.ts > fact_event.ts ? event.ts - fact_event.ts : fact_event.ts - event.ts;
        if (delta <= kNearbyAnchorWindowNs) return true;
    }
    return false;
}

Json build_anchor_coverage(const core::DagGraph & graph) {
    constexpr std::string_view roles[] = { "cache_lookup_input", "cache_extend_input", "cache_lifecycle_commit", "prefetch_candidate_anchor" };
    std::map<std::string, RoleAnchorStats> stats;
    for (auto role : roles) stats[std::string(role)] = RoleAnchorStats{};

    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        auto fact = parse_fact(event);
        if (!fact.present || fact.fact_class != "workload_identity" || !stats.contains(fact.role)) continue;
        auto & row = stats[fact.role];
        row.fact_count++;
        const bool nearby = has_near_runtime_anchor(graph, event);
        row.direct_fact_node_count++;
        if (nearby) row.near_runtime_anchor_count++;
    }

    Json roles_json = Json::object();
    Json blockers = Json::array();
    for (const auto & item : stats) {
        const auto & row = item.second;
        roles_json[item.first] = Json{
            {                  "fact_count",                  row.fact_count },
            {      "direct_fact_node_count",      row.direct_fact_node_count },
            {   "near_runtime_anchor_count",   row.near_runtime_anchor_count },
            { "timestamp_only_anchor_count", row.timestamp_only_anchor_count },
            {        "missing_anchor_count",        row.missing_anchor_count },
            {              "sample_missing",              row.sample_missing },
        };
        if (row.fact_count == 0) blockers.push_back("role_fact_missing:" + item.first);
        if (row.missing_anchor_count > 0) blockers.push_back("anchor_missing:" + item.first);
        if (row.timestamp_only_anchor_count > 0) blockers.push_back("timestamp_only_anchor:" + item.first);
    }

    return Json{
        {         "schema","markov.trace_graph.dag_anchor_coverage.v1"                           },
        {     "fact_roles",                                  roles_json },
        { "anchor_methods",
         Json{
         { "direct_python_probe_node", sum_role_value(stats, &RoleAnchorStats::direct_fact_node_count) },
         { "same_request_nearby_runtime_node", sum_role_value(stats, &RoleAnchorStats::near_runtime_anchor_count) },
         { "timestamp_only", sum_role_value(stats, &RoleAnchorStats::timestamp_only_anchor_count) },
         }                                                             },
        {          "ready",                            blockers.empty() },
        {       "blockers",                                    blockers },
    };
}

size_t sum_role_value(const std::map<std::string, RoleAnchorStats> & stats, size_t RoleAnchorStats::*member) {
    size_t total = 0;
    for (const auto & item : stats) total += item.second.*member;
    return total;
}

bool operation_match(const std::string & text, std::initializer_list<std::string_view> needles) {
    return std::ranges::any_of(needles, [&](std::string_view needle) { return contains(text, needle); });
}

std::vector<OperationEvidence> collect_operation_evidence(const core::DagGraph & graph) {
    std::vector<OperationEvidence> operations = {
        { "device_allocation_or_eviction", {}, {}, {} },
        {                   "host_backup", {}, {}, {} },
        {                 "storage_write", {}, {}, {} },
        {         "storage_prefetch_read", {}, {}, {} },
        {           "prefetch_apply_host", {}, {}, {} },
        {               "device_loadback", {}, {}, {} },
        {    "host_cleanup_release_drain", {}, {}, {} },
        {        "writeback_ack_or_drain", {}, {}, {} },
        {      "prefetch_wait_or_timeout", {}, {}, {} },
        {      "scheduler_radix_cpu_work", {}, {}, {} },
    };

    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        const auto fact = parse_fact(event);
        auto text = lower(event.name + " " + event.cat + " " + event.arg("event_kind") + " " + fact.role + " " + event.arg("target"));
        auto observe = [&](const std::string & operation, bool direct_match, bool control_match, bool state_match) {
            auto it = std::ranges::find_if(operations, [&](const OperationEvidence & item) { return item.name == operation; });
            if (it == operations.end()) return;
            if (direct_match && is_runtime_node(event)) it->direct_runtime_nodes.push_back(node.id);
            if (control_match && fact.present) it->control_fact_nodes.push_back(node.id);
            if (state_match && fact.present) it->state_fact_nodes.push_back(node.id);
        };

        observe("device_allocation_or_eviction", operation_match(text, { "alloc", "evict", "revoke" }), fact.role == "capacity_result_observed", false);
        observe("host_backup", operation_match(text, { "backup", "host" }), operation_match(text, { "writeback", "write_back", "backup" }), false);
        observe("storage_write", operation_match(text, { "storage", "writeback", "write_back", "write" }), fact.role == "writeback_io_observed", false);
        observe("storage_prefetch_read", operation_match(text, { "prefetch", "storage", "read" }), fact.role == "prefetch_io_observed", false);
        observe("prefetch_apply_host",
                operation_match(text, { "prefetch", "host", "progress" }),
                operation_match(text, { "prefetch_progress", "prefetch_intent" }),
                false);
        observe("device_loadback", operation_match(text, { "loadback", "load_back", "device" }), operation_match(text, { "insert_result", "capacity" }), false);
        observe("host_cleanup_release_drain",
                operation_match(text, { "cleanup", "release", "drain" }),
                operation_match(text, { "lock_scope", "request_admission" }),
                false);
        observe("writeback_ack_or_drain",
                operation_match(text, { "ack", "writeback", "drain" }),
                operation_match(text, { "writeback_enqueue", "writeback_io" }),
                false);
        observe("prefetch_wait_or_timeout",
                operation_match(text, { "wait", "timeout", "prefetch" }),
                operation_match(text, { "prefetch_progress", "prefetch_decision" }),
                false);
        observe("scheduler_radix_cpu_work",
                operation_match(text, { "match_prefix", "prepare_for_extend", "_prefetch_kvcache", "radix" }),
                fact.fact_class == "workload_identity",
                fact.fact_class == "workload_identity");
    }
    return operations;
}

Json node_samples(const core::DagGraph & graph, const std::vector<size_t> & node_ids) {
    Json result = Json::array();
    for (size_t i = 0; i < node_ids.size() && i < kSampleLimit; ++i) {
        size_t node_id = node_ids[i];
        result.push_back(compact_node_json(graph, node_id));
    }
    return result;
}

Json operation_json(const core::DagGraph & graph, const OperationEvidence & evidence) {
    std::string visibility = "invisible";
    std::string duration_source = "none";
    if (!evidence.direct_runtime_nodes.empty()) {
        visibility = "visible";
        duration_source = "runtime_node";
    }
    else if (!evidence.control_fact_nodes.empty() || !evidence.state_fact_nodes.empty()) {
        visibility = "partially_visible";
        duration_source = evidence.control_fact_nodes.empty() ? "state_fact_only" : "control_fact";
    }
    Json blockers = Json::array();
    if (visibility == "invisible") blockers.push_back("operation_invisible");
    if (duration_source == "none" || duration_source == "state_fact_only" || duration_source == "control_fact") blockers.push_back("duration_source_missing");
    return Json{
        {          "visibility",                                                                           visibility                                },
        {            "evidence",
         Json{
         { "direct_runtime_node_count", evidence.direct_runtime_nodes.size() },
         { "control_fact_node_count", evidence.control_fact_nodes.size() },
         { "state_fact_node_count", evidence.state_fact_nodes.size() },
         { "direct_runtime_node_samples", node_samples(graph, evidence.direct_runtime_nodes) },
         { "control_fact_node_samples", node_samples(graph, evidence.control_fact_nodes) },
         }                                                                                                                                           },
        {    "possible_anchors", evidence.direct_runtime_nodes.empty() ? Json::array({ "state_fact_or_timestamp" }) : Json::array({ "runtime_node" }) },
        {     "duration_source",                                                                                                      duration_source },
        { "patchable_candidate",                                                                                              visibility == "visible" },
        {            "blockers",                                                                                                             blockers },
    };
}

Json build_operation_visibility(const core::DagGraph & graph) {
    Json operations_json = Json::object();
    Json summary = {
        {           "visible_count", 0 },
        { "partially_visible_count", 0 },
        {         "invisible_count", 0 },
        {           "unknown_count", 0 },
    };
    for (const auto & evidence : collect_operation_evidence(graph)) {
        auto row = operation_json(graph, evidence);
        auto visibility = row.value("visibility", "unknown");
        summary[visibility + "_count"] = summary.value(visibility + "_count", 0) + 1;
        operations_json[evidence.name] = std::move(row);
    }
    return Json{
        {     "schema", "markov.trace_graph.dag_operation_visibility.v1" },
        { "operations",                                  operations_json },
        {    "summary",                                          summary },
    };
}

} // namespace dag_analysis_detail

using dag_analysis_detail::build_anchor_coverage;
using dag_analysis_detail::build_dag_analysis;
using dag_analysis_detail::build_dag_quality;
using dag_analysis_detail::build_operation_visibility;

DagAnalysisArtifacts build_dag_analysis_artifacts(const core::DagGraph & graph) {
    return DagAnalysisArtifacts{
        .dag_quality_json = build_dag_quality(graph).dump(2),
        .dag_analysis_json = build_dag_analysis(graph).dump(2),
        .dag_anchor_coverage_json = build_anchor_coverage(graph).dump(2),
        .dag_operation_visibility_json = build_operation_visibility(graph).dump(2),
    };
}

} // namespace markov::trace_graph::modules::dag_analysis
