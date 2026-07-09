/**
 * @file
 * @brief 阶段一 DAG analysis JSON artifact 构造实现。
 */
#include "markov/trace_graph/modules/dag_analysis/diagnostics.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <future>
#include <initializer_list>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
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
constexpr size_t kCriticalPathTopIntervalLimit = 64;
constexpr uint64_t kNearbyAnchorWindowNs = 1'000'000;
constexpr size_t kSampleLimit = 8;
constexpr size_t kCycleWitnessNodeLimit = 256;
constexpr size_t kCycleRemainingSampleLimit = 64;
constexpr size_t kInvalidNode = std::numeric_limits<size_t>::max();
constexpr uint64_t kMaxCpuIntervalNs = 1'000'000'000'000ull;

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

struct LaneStats {
    size_t node_count = 0;
    uint64_t duration_ns = 0;
};

struct LaneSummaryStats {
    std::map<std::string, LaneStats> cpu;
    std::map<std::string, LaneStats> device;
    std::map<std::string, LaneStats> hccl;
};

struct AnchorFact {
    std::string role;
    uint64_t timestamp_ns = 0;
};

struct EvidenceNodes {
    size_t count = 0;
    uint64_t duration_ns = 0;
    std::vector<size_t> samples;
};

struct OperationEvidence {
    std::string name;
    EvidenceNodes direct_runtime_nodes;
    EvidenceNodes control_fact_nodes;
    EvidenceNodes state_fact_nodes;
};

struct SyncCoverageStats {
    size_t event_wait_nodes = 0;
    size_t stream_sync_nodes = 0;
    size_t event_sync_nodes = 0;
    size_t device_sync_nodes = 0;
    size_t notify_wait_nodes = 0;
    size_t sync_edges = 0;
};

struct EdgeRef {
    size_t edge_index = kInvalidNode;
    size_t src = kInvalidNode;
    size_t dst = kInvalidNode;
    core::DagEdgeKind kind = core::DagEdgeKind::Sequential;
};

struct DfsFrame {
    size_t node_id = kInvalidNode;
    size_t next_edge_index = 0;
};

struct CycleWitness {
    size_t processed_nodes = 0;
    size_t remaining_node_count = 0;
    std::vector<size_t> remaining_node_sample;
    std::vector<size_t> cycle_nodes;
    std::vector<EdgeRef> cycle_edges;
};

struct DagAnalysisScan {
    std::map<std::string, size_t> by_source;
    std::map<std::string, size_t> by_domain;
    std::map<std::string, size_t> by_category;
    SyncCoverageStats sync_coverage;
    LaneSummaryStats lane_summary;
    std::unordered_set<std::string> cpu_lanes;
    std::unordered_set<std::string> device_lanes;
    std::unordered_set<std::string> correlation_groups;
    std::unordered_set<std::string> connection_groups;
    std::vector<uint64_t> runtime_ts;
    std::vector<AnchorFact> anchor_facts;
    std::vector<OperationEvidence> operations;
};

size_t sum_role_value(const std::map<std::string, RoleAnchorStats> & stats, size_t RoleAnchorStats::*member);

uint64_t elapsed_ms(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

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

bool raw_contains_key_hint(const core::TraceEvent & event, std::string_view key) {
    auto raw = event.args_json_view();
    return raw.find(key) != std::string_view::npos;
}

std::string event_domain(const core::TraceEvent & event) {
    if (raw_contains_key_hint(event, "domain")) {
        auto domain = event.arg("domain");
        if (!domain.empty()) return domain;
    }
    if (!event.cat.empty()) return event.cat;
    return "unknown";
}

std::string event_source(const core::TraceEvent & event) {
    if (raw_contains_key_hint(event, "trace_source")) {
        auto source = event.arg("trace_source");
        if (!source.empty()) return source;
    }
    if (raw_contains_key_hint(event, "source")) {
        auto source = event.arg("source");
        if (!source.empty()) return source;
    }
    if (raw_contains_key_hint(event, "domain")) {
        auto domain = event.arg("domain");
        if (domain == "python_probe") return "python_probe";
        if (!domain.empty()) return domain;
    }
    if (event.name.starts_with("AscendCL@") || event.name == "EVENT_RECORD" || event.name == "EVENT_WAIT") return "ld_preload";
    if (raw_contains_key_hint(event, "Physic Stream Id") || event.cat == "Kernel") return "torch_or_device";
    if (!event.cat.empty()) return event.cat;
    return "unknown";
}

bool is_python_probe_fact_node(const core::TraceEvent & event) {
    if (event.cat == "hicache" || event.name.starts_with("hicache_")) return true;
    return raw_contains_key_hint(event, "python_probe") && event.arg("domain") == "python_probe";
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

Json selected_args_json(const core::TraceEvent & event) {
    constexpr std::string_view keys[] = {
        "correlation_id",
        "connection_id",
        "Raw Stream",
        "streamId",
        "stream id",
        "Physic Stream Id",
        "Event Id",
        "event_id",
        "launchts",
        "submitts",
        "submit_anchor_name",
        "submit_anchor_source",
        "trace_source",
        "source",
        "domain",
    };
    Json result = Json::object();
    for (auto key : keys) {
        auto value = event.arg(std::string(key));
        if (!value.empty()) result[std::string(key)] = value;
    }
    return result;
}

Json cycle_node_json(const core::DagGraph & graph, size_t node_id) {
    auto item = compact_node_json(graph, node_id);
    const auto & node = graph.node(node_id);
    const auto & event = graph.event_for_node(node_id);
    item["gpu_id"] = node.gpu_id;
    item["source"] = event_source(event);
    item["pid"] = event.pid;
    item["tid"] = event.tid;
    item["ts_ns"] = event.ts;
    item["event_duration_ns"] = event.dur;
    item["args"] = selected_args_json(event);
    return item;
}

uint64_t event_submit_ts(const core::TraceEvent & event) {
    auto submit_ts = event.arg_u64("submitts", 0);
    return submit_ts > 0 ? submit_ts : event.ts;
}

Json edge_ref_json(const core::DagGraph & graph, const EdgeRef & edge) {
    const auto & src_event = graph.event_for_node(edge.src);
    const auto & dst_event = graph.event_for_node(edge.dst);
    const auto src_submit_ts = event_submit_ts(src_event);
    const auto dst_submit_ts = event_submit_ts(dst_event);
    Json delta = nullptr;
    if (dst_event.ts >= src_event.ts) delta = dst_event.ts - src_event.ts;
    Json submit_delta = nullptr;
    if (dst_submit_ts >= src_submit_ts) submit_delta = dst_submit_ts - src_submit_ts;
    return Json{
        {              "edge_index",          edge.edge_index },
        {                    "kind", edge_kind_name(edge.kind) },
        {                     "src",                  edge.src },
        {                     "dst",                  edge.dst },
        {                "src_name",            src_event.name },
        {                "dst_name",            dst_event.name },
        {                "src_lane",    graph.node(edge.src).lane_key },
        {                "dst_lane",    graph.node(edge.dst).lane_key },
        {               "src_ts_ns",             src_event.ts },
        {               "dst_ts_ns",             dst_event.ts },
        {          "src_submit_ts_ns",           src_submit_ts },
        {          "dst_submit_ts_ns",           dst_submit_ts },
        {      "dst_minus_src_ts_ns",                 delta },
        { "dst_minus_src_submit_ts_ns",          submit_delta },
        {             "backward_ts",      dst_event.ts < src_event.ts },
        {    "backward_submit_ts",       dst_submit_ts < src_submit_ts },
    };
}

Json count_map_json(const std::map<std::string, size_t> & counts) {
    Json result = Json::object();
    for (const auto & item : counts) result[item.first] = item.second;
    return result;
}

Json unordered_count_map_json(const std::unordered_map<std::string, size_t> & counts) {
    Json result = Json::object();
    std::vector<std::string> keys;
    keys.reserve(counts.size());
    for (const auto & item : counts) keys.push_back(item.first);
    std::ranges::sort(keys);
    for (const auto & key : keys) result[key] = counts.at(key);
    return result;
}

CycleWitness find_cycle_witness(const core::DagGraph & graph) {
    const size_t node_count = graph.node_count();
    std::vector<std::vector<EdgeRef>> outgoing(node_count);
    std::vector<int> indegree(node_count, 0);
    const auto & edges = graph.edges();
    for (size_t edge_index = 0; edge_index < edges.size(); ++edge_index) {
        const auto & edge = edges[edge_index];
        if (edge.src >= node_count || edge.dst >= node_count) continue;
        outgoing[edge.src].push_back(EdgeRef{ .edge_index = edge_index, .src = edge.src, .dst = edge.dst, .kind = edge.kind });
        indegree[edge.dst]++;
    }

    std::queue<size_t> ready;
    for (size_t node_id = 0; node_id < node_count; ++node_id) {
        if (indegree[node_id] == 0) ready.push(node_id);
    }

    CycleWitness witness;
    while (!ready.empty()) {
        const auto node_id = ready.front();
        ready.pop();
        witness.processed_nodes++;
        for (const auto & edge : outgoing[node_id]) {
            indegree[edge.dst]--;
            if (indegree[edge.dst] == 0) ready.push(edge.dst);
        }
    }

    for (size_t node_id = 0; node_id < node_count; ++node_id) {
        if (indegree[node_id] <= 0) continue;
        witness.remaining_node_count++;
        if (witness.remaining_node_sample.size() < kCycleRemainingSampleLimit) witness.remaining_node_sample.push_back(node_id);
    }
    if (witness.remaining_node_count == 0) return witness;

    std::vector<int> visit_state(node_count, 0);
    std::vector<size_t> path_nodes;
    std::vector<EdgeRef> path_in_edges;
    std::vector<size_t> path_position(node_count, kInvalidNode);
    std::vector<DfsFrame> dfs_stack;

    for (size_t start = 0; start < node_count; ++start) {
        if (indegree[start] <= 0 || visit_state[start] != 0) continue;
        path_nodes.clear();
        path_in_edges.clear();
        dfs_stack.clear();
        dfs_stack.push_back(DfsFrame{ .node_id = start, .next_edge_index = 0 });
        visit_state[start] = 1;
        path_position[start] = path_nodes.size();
        path_nodes.push_back(start);
        path_in_edges.push_back(EdgeRef{});

        while (!dfs_stack.empty()) {
            auto & frame = dfs_stack.back();
            const auto & edges_for_node = outgoing[frame.node_id];
            bool descended = false;
            while (frame.next_edge_index < edges_for_node.size()) {
                const auto edge = edges_for_node[frame.next_edge_index++];
                if (edge.dst >= node_count || indegree[edge.dst] <= 0) continue;
                if (visit_state[edge.dst] == 0) {
                    visit_state[edge.dst] = 1;
                    path_position[edge.dst] = path_nodes.size();
                    path_nodes.push_back(edge.dst);
                    path_in_edges.push_back(edge);
                    dfs_stack.push_back(DfsFrame{ .node_id = edge.dst, .next_edge_index = 0 });
                    descended = true;
                    break;
                }
                if (visit_state[edge.dst] == 1) {
                    const auto position = path_position[edge.dst];
                    if (position != kInvalidNode && position < path_nodes.size()) {
                        witness.cycle_nodes.assign(path_nodes.begin() + static_cast<std::ptrdiff_t>(position), path_nodes.end());
                        witness.cycle_edges.assign(path_in_edges.begin() + static_cast<std::ptrdiff_t>(position + 1), path_in_edges.end());
                        witness.cycle_edges.push_back(edge);
                    }
                    else {
                        witness.cycle_nodes = { edge.dst };
                        witness.cycle_edges = { edge };
                    }
                    return witness;
                }
            }
            if (descended) continue;

            const auto done = frame.node_id;
            dfs_stack.pop_back();
            visit_state[done] = 2;
            path_position[done] = kInvalidNode;
            if (!path_nodes.empty()) path_nodes.pop_back();
            if (!path_in_edges.empty()) path_in_edges.pop_back();
        }
    }
    return witness;
}

void add_lane_summary_node(LaneSummaryStats & summary, const core::DagNode & node, const core::TraceEvent & event) {
    auto & lanes = node.is_cpu ? summary.cpu : summary.device;
    auto & item = lanes[node.lane_key];
    item.node_count++;
    item.duration_ns += node.duration;
    if (!node.is_cpu && (event.name.contains("hcom") || event.name.contains("HCCL") || event.name.contains("hccl"))) {
        auto & hccl_item = summary.hccl[node.lane_key];
        hccl_item.node_count++;
        hccl_item.duration_ns += node.duration;
    }
}

Json lane_stats_json(const std::map<std::string, LaneStats> & stats) {
    Json result = Json::object();
    for (const auto & item : stats) {
        result[item.first] = Json{
            {  "node_count",     item.second.node_count },
            { "duration_ns", item.second.duration_ns }
        };
    }
    return result;
}

Json lane_summary_json(const LaneSummaryStats & summary) {
    return Json{
        {    "cpu",    lane_stats_json(summary.cpu) },
        { "device", lane_stats_json(summary.device) },
        {   "hccl",   lane_stats_json(summary.hccl) }
    };
}

uint64_t read_u64_attr(const core::DagNode & node, const char * key, uint64_t fallback = 0) {
    auto it = node.attrs.find(key);
    if (it == node.attrs.end()) return fallback;
    uint64_t value = 0;
    const auto & text = it->second;
    const auto * first = text.data();
    const auto * last = first + text.size();
    auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) return fallback;
    return value;
}

void add_json_counter(Json & target, const std::string & key, uint64_t value) { target[key] = target.value(key, 0ULL) + value; }

void add_critical_path_node(Json & counts_by_source, Json & durations_by_source, Json & counts_by_domain, Json & durations_by_domain, Json & counts_by_category,
                            Json & durations_by_category, const core::DagGraph & graph, size_t node_id) {
    const auto & node = graph.node(node_id);
    const auto & event = graph.event_for_node(node_id);
    const auto source = event_source(event);
    const auto domain = event_domain(event);
    const auto category = event.cat.empty() ? std::string("unknown") : event.cat;
    add_json_counter(counts_by_source, source, 1);
    add_json_counter(durations_by_source, source, node.duration);
    add_json_counter(counts_by_domain, domain, 1);
    add_json_counter(durations_by_domain, domain, node.duration);
    add_json_counter(counts_by_category, category, 1);
    add_json_counter(durations_by_category, category, node.duration);
}

void add_critical_path_interval(Json & intervals_by_source, Json & intervals_by_domain, Json & intervals_by_category, const core::DagGraph & graph,
                                size_t node_id, uint64_t interval_ns) {
    const auto & event = graph.event_for_node(node_id);
    const auto source = event_source(event);
    const auto domain = event_domain(event);
    const auto category = event.cat.empty() ? std::string("unknown") : event.cat;
    add_json_counter(intervals_by_source, source, interval_ns);
    add_json_counter(intervals_by_domain, domain, interval_ns);
    add_json_counter(intervals_by_category, category, interval_ns);
}

Json critical_path_sample(const core::DagGraph & graph) {
    constexpr size_t invalid = std::numeric_limits<size_t>::max();
    if (graph.node_count() == 0)
        return Json{
            { "total_duration_ns", graph.e2e_time() },
            {      "sample_nodes",    Json::array() }
        };

    std::vector<size_t> best_pred_by_node(graph.node_count(), invalid);
    std::vector<core::DagEdgeKind> best_kind_by_node(graph.node_count(), core::DagEdgeKind::Sequential);
    for (const auto & edge : graph.edges()) {
        if (edge.src >= graph.node_count() || edge.dst >= graph.node_count()) continue;
        const auto & pred_node = graph.node(edge.src);
        auto current_pred = best_pred_by_node[edge.dst];
        if (current_pred == invalid || pred_node.completion_time > graph.node(current_pred).completion_time) {
            best_pred_by_node[edge.dst] = edge.src;
            best_kind_by_node[edge.dst] = edge.kind;
        }
    }

    size_t current = 0;
    for (const auto & node : graph.nodes()) {
        if (node.completion_time > graph.node(current).completion_time) current = node.id;
    }

    std::vector<Json> reversed;
    std::vector<Json> top_cpu_intervals;
    std::unordered_set<size_t> seen;
    Json node_counts_by_source = Json::object();
    Json duration_by_source = Json::object();
    Json node_counts_by_domain = Json::object();
    Json duration_by_domain = Json::object();
    Json node_counts_by_category = Json::object();
    Json duration_by_category = Json::object();
    Json cpu_interval_by_source = Json::object();
    Json cpu_interval_by_domain = Json::object();
    Json cpu_interval_by_category = Json::object();
    Json edge_counts_by_kind = Json::object();
    size_t path_node_count = 0;
    uint64_t path_node_duration_ns = 0;
    uint64_t path_cpu_interval_ns = 0;
    uint64_t cpu_duration_ns = 0;
    uint64_t device_duration_ns = 0;
    while (current != invalid && seen.insert(current).second) {
        const auto & node = graph.node(current);
        path_node_count++;
        path_node_duration_ns += node.duration;
        if (node.is_cpu) cpu_duration_ns += node.duration;
        else device_duration_ns += node.duration;
        add_critical_path_node(node_counts_by_source,
                               duration_by_source,
                               node_counts_by_domain,
                               duration_by_domain,
                               node_counts_by_category,
                               duration_by_category,
                               graph,
                               current);

        size_t best_pred = best_pred_by_node[current];
        std::string incoming_edge_kind;
        if (best_pred != invalid) {
            incoming_edge_kind = edge_kind_name(best_kind_by_node[current]);
            add_json_counter(edge_counts_by_kind, incoming_edge_kind, 1);
            const auto & pred_node = graph.node(best_pred);
            const auto interval = read_u64_attr(pred_node, "cpuinterval", 0);
            if (interval <= kMaxCpuIntervalNs) {
                path_cpu_interval_ns += interval;
                add_critical_path_interval(cpu_interval_by_source, cpu_interval_by_domain, cpu_interval_by_category, graph, best_pred, interval);
                if (interval > 0) {
                    auto interval_item = compact_node_json(graph, best_pred);
                    interval_item["cpuinterval_ns"] = interval;
                    interval_item["edge_to_node_id"] = current;
                    interval_item["edge_to_name"] = graph.event_for_node(current).name;
                    interval_item["edge_to_incoming_kind"] = incoming_edge_kind;
                    top_cpu_intervals.push_back(std::move(interval_item));
                }
            }
        }

        auto item = compact_node_json(graph, current);
        if (!incoming_edge_kind.empty()) item["incoming_edge_kind"] = incoming_edge_kind;
        if (reversed.size() < kCriticalPathSampleLimit) reversed.push_back(std::move(item));

        if (best_pred == invalid) break;
        current = best_pred;
    }
    std::ranges::reverse(reversed);
    std::ranges::sort(top_cpu_intervals, [](const Json & lhs, const Json & rhs) {
        return lhs.value("cpuinterval_ns", uint64_t{ 0 }) > rhs.value("cpuinterval_ns", uint64_t{ 0 });
    });
    if (top_cpu_intervals.size() > kCriticalPathTopIntervalLimit) top_cpu_intervals.resize(kCriticalPathTopIntervalLimit);
    return Json{
        {        "total_duration_ns",                             graph.e2e_time() },
        {          "path_node_count",                              path_node_count },
        {    "path_node_duration_ns",                        path_node_duration_ns },
        {     "path_cpu_interval_ns",                         path_cpu_interval_ns },
        {     "path_modeled_cost_ns", path_node_duration_ns + path_cpu_interval_ns },
        {          "cpu_duration_ns",                              cpu_duration_ns },
        {       "device_duration_ns",                           device_duration_ns },
        {     "node_count_by_source",                        node_counts_by_source },
        {       "duration_by_source",                           duration_by_source },
        {   "cpu_interval_by_source",                       cpu_interval_by_source },
        {     "node_count_by_domain",                        node_counts_by_domain },
        {       "duration_by_domain",                           duration_by_domain },
        {   "cpu_interval_by_domain",                       cpu_interval_by_domain },
        {   "node_count_by_category",                      node_counts_by_category },
        {     "duration_by_category",                         duration_by_category },
        { "cpu_interval_by_category",                     cpu_interval_by_category },
        {       "edge_count_by_kind",                          edge_counts_by_kind },
        {             "sample_nodes",                                     reversed },
        {        "top_cpu_intervals",                            top_cpu_intervals },
    };
}

Json build_dag_quality(const core::DagGraph & graph, const std::unordered_map<std::string, size_t> & edge_counts, const DagAnalysisScan & scan) {
    const auto real = graph.real_e2e_time();
    const auto simulated = graph.e2e_time();
    const auto abs_error = real > simulated ? real - simulated : simulated - real;
    Json rel_error = nullptr;
    if (real > 0) rel_error = static_cast<double>(abs_error) / static_cast<double>(real);
    Json blockers = Json::array();
    if (real == 0) blockers.push_back("missing_trace_real_e2e");
    if (real > 0 && static_cast<double>(abs_error) / static_cast<double>(real) > kFaithfulReplayRelErrorMax) blockers.push_back("dag_replay_error_too_high");

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
         { "edge_counts_by_kind", edge_counts },
         { "cpu_lane_count", scan.cpu_lanes.size() },
         { "device_lane_count", scan.device_lanes.size() },
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

Json build_dag_analysis(const core::DagGraph & graph, const std::unordered_map<std::string, size_t> & edge_counts, const DagAnalysisScan & scan) {
    const auto correlation_edges = edge_counts.contains("correlation") ? edge_counts.at("correlation") : 0;
    Json correlation_coverage = {
        { "correlation_groups",                 0 },
        {  "connection_groups",                 0 },
        {  "correlation_edges", correlation_edges },
    };
    correlation_coverage["correlation_groups"] = scan.correlation_groups.size();
    correlation_coverage["connection_groups"] = scan.connection_groups.size();
    Json blockers = Json::array();
    if (graph.node_count() == 0) blockers.push_back("empty_dag");
    if (graph.edge_count() == 0 && graph.node_count() > 1) blockers.push_back("dag_edge_coverage_gap");

    return Json{
        {                 "schema", "markov.trace_graph.dag_analysis.v1" },
        {   "node_count_by_source",          count_map_json(scan.by_source) },
        {   "node_count_by_domain",          count_map_json(scan.by_domain) },
        { "node_count_by_category",        count_map_json(scan.by_category) },
        {     "edge_count_by_kind",                          edge_counts },
        {           "lane_summary",  lane_summary_json(scan.lane_summary) },
        {          "sync_coverage",
         Json{
         { "event_wait_nodes", scan.sync_coverage.event_wait_nodes },
         { "stream_sync_nodes", scan.sync_coverage.stream_sync_nodes },
         { "event_sync_nodes", scan.sync_coverage.event_sync_nodes },
         { "device_sync_nodes", scan.sync_coverage.device_sync_nodes },
         { "notify_wait_nodes", scan.sync_coverage.notify_wait_nodes },
         { "sync_edges", scan.sync_coverage.sync_edges },
         }                                                            },
        {   "correlation_coverage",                 correlation_coverage },
        {          "critical_path",          critical_path_sample(graph) },
        {               "blockers",                             blockers },
    };
}

bool has_near_runtime_anchor(const std::vector<uint64_t> & runtime_ts, uint64_t fact_ts) {
    auto lower_bound = fact_ts > kNearbyAnchorWindowNs ? fact_ts - kNearbyAnchorWindowNs : 0;
    auto it = std::lower_bound(runtime_ts.begin(), runtime_ts.end(), lower_bound);
    if (it == runtime_ts.end()) return false;
    auto delta = *it > fact_ts ? *it - fact_ts : fact_ts - *it;
    if (delta <= kNearbyAnchorWindowNs) return true;
    ++it;
    if (it == runtime_ts.end()) return false;
    delta = *it > fact_ts ? *it - fact_ts : fact_ts - *it;
    return delta <= kNearbyAnchorWindowNs;
}

bool role_tracked(std::string_view role) {
    return role == "cache_lookup_input" || role == "cache_extend_input" || role == "cache_lifecycle_commit" || role == "prefetch_candidate_anchor";
}

RoleAnchorStats & role_stats(std::map<std::string, RoleAnchorStats> & stats, const std::string & role) {
    auto it = stats.find(role);
    if (it != stats.end()) return it->second;
    return stats.emplace(role, RoleAnchorStats{}).first->second;
}

bool fact_role_relevant(const core::TraceEvent & event) {
    auto raw = event.args_json_view();
    return raw.find("workload_identity") != std::string_view::npos
           && (raw.find("cache_lookup_input") != std::string_view::npos || raw.find("cache_extend_input") != std::string_view::npos
               || raw.find("cache_lifecycle_commit") != std::string_view::npos || raw.find("prefetch_candidate_anchor") != std::string_view::npos);
}

std::string raw_fact_role_hint(const core::TraceEvent & event) {
    auto raw = event.args_json_view();
    if (raw.find("cache_lookup_input") != std::string_view::npos) return "cache_lookup_input";
    if (raw.find("cache_extend_input") != std::string_view::npos) return "cache_extend_input";
    if (raw.find("cache_lifecycle_commit") != std::string_view::npos) return "cache_lifecycle_commit";
    if (raw.find("prefetch_candidate_anchor") != std::string_view::npos) return "prefetch_candidate_anchor";
    return "";
}

std::string raw_fact_class_hint(const core::TraceEvent & event) {
    auto raw = event.args_json_view();
    if (raw.find("workload_identity") != std::string_view::npos) return "workload_identity";
    return "";
}

FactInfo parse_relevant_fact(const core::TraceEvent & event) {
    FactInfo info;
    if (!fact_role_relevant(event)) return info;
    if (!event.has_arg("fact")) return info;
    info.fact_class = raw_fact_class_hint(event);
    info.role = raw_fact_role_hint(event);
    if (!info.fact_class.empty() && !info.role.empty()) {
        info.present = true;
        return info;
    }
    return parse_fact(event);
}

bool operation_fact_candidate(const core::TraceEvent & event) {
    auto raw = event.args_json_view();
    return raw.find("capacity_result_observed") != std::string_view::npos || raw.find("writeback_io_observed") != std::string_view::npos
           || raw.find("prefetch_io_observed") != std::string_view::npos || raw.find("prefetch_progress") != std::string_view::npos
           || raw.find("prefetch_intent") != std::string_view::npos || raw.find("insert_result") != std::string_view::npos || raw.find("capacity") != std::string_view::npos
           || raw.find("lock_scope") != std::string_view::npos || raw.find("request_admission") != std::string_view::npos
           || raw.find("writeback_enqueue") != std::string_view::npos || raw.find("writeback_io") != std::string_view::npos
           || raw.find("prefetch_decision") != std::string_view::npos || raw.find("workload_identity") != std::string_view::npos;
}

FactInfo parse_operation_fact(const core::TraceEvent & event) {
    if (!operation_fact_candidate(event) || !event.has_arg("fact")) return FactInfo{};
    return parse_fact(event);
}

bool name_contains_any(const std::string & name, std::initializer_list<std::string_view> needles) {
    return std::ranges::any_of(needles, [&](std::string_view needle) { return contains(name, needle); });
}

std::string lower_if_needed(const core::TraceEvent & event) {
    auto raw = event.args_json_view();
    if (!name_contains_any(event.name, { "alloc", "evict", "revoke", "backup", "host", "storage", "writeback", "write_back", "write", "prefetch", "read",
                                         "progress", "loadback", "load_back", "device", "cleanup", "release", "drain", "ack", "wait", "timeout",
                                         "match_prefix", "prepare_for_extend", "_prefetch_kvcache", "radix" })
        && !name_contains_any(event.cat, { "alloc", "evict", "revoke", "backup", "host", "storage", "writeback", "write_back", "write", "prefetch", "read",
                                           "progress", "loadback", "load_back", "device", "cleanup", "release", "drain", "ack", "wait", "timeout",
                                           "match_prefix", "prepare_for_extend", "_prefetch_kvcache", "radix" })
        && raw.find("fact") == std::string_view::npos && raw.find("event_kind") == std::string_view::npos && raw.find("target") == std::string_view::npos)
        return "";
    return lower(event.name + " " + event.cat + " " + event.arg("event_kind") + " " + event.arg("target"));
}
Json build_anchor_coverage(const DagAnalysisScan & scan) {
    constexpr std::string_view roles[] = { "cache_lookup_input", "cache_extend_input", "cache_lifecycle_commit", "prefetch_candidate_anchor" };
    std::map<std::string, RoleAnchorStats> stats;
    for (auto role : roles) stats[std::string(role)] = RoleAnchorStats{};

    for (const auto & fact : scan.anchor_facts) {
        auto & row = role_stats(stats, fact.role);
        row.fact_count++;
        row.direct_fact_node_count++;
        if (has_near_runtime_anchor(scan.runtime_ts, fact.timestamp_ns)) row.near_runtime_anchor_count++;
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

void record_evidence(EvidenceNodes & nodes, const core::DagNode & node) {
    nodes.count++;
    nodes.duration_ns += node.duration;
    if (nodes.samples.size() < kSampleLimit) nodes.samples.push_back(node.id);
}

std::vector<OperationEvidence> make_operation_evidence() {
    return {
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
}

enum OperationIndex : size_t {
    kDeviceAllocationOrEviction = 0,
    kHostBackup = 1,
    kStorageWrite = 2,
    kStoragePrefetchRead = 3,
    kPrefetchApplyHost = 4,
    kDeviceLoadback = 5,
    kHostCleanupReleaseDrain = 6,
    kWritebackAckOrDrain = 7,
    kPrefetchWaitOrTimeout = 8,
    kSchedulerRadixCpuWork = 9,
};

void observe_operation_event(std::vector<OperationEvidence> & operations, const core::DagNode & node, const core::TraceEvent & event) {
    const auto fact = parse_operation_fact(event);
    auto text = lower_if_needed(event);
    if (!fact.role.empty()) {
        if (!text.empty()) text.push_back(' ');
        text += fact.role;
    }
    if (text.empty() && !fact.present) return;
    auto observe = [&](size_t operation_index, bool direct_match, bool control_match, bool state_match) {
        auto & operation = operations[operation_index];
        if (direct_match && is_runtime_node(event)) record_evidence(operation.direct_runtime_nodes, node);
        if (control_match && fact.present) record_evidence(operation.control_fact_nodes, node);
        if (state_match && fact.present) record_evidence(operation.state_fact_nodes, node);
    };

    observe(kDeviceAllocationOrEviction, operation_match(text, { "alloc", "evict", "revoke" }), fact.role == "capacity_result_observed", false);
    observe(kHostBackup, operation_match(text, { "backup", "host" }), operation_match(text, { "writeback", "write_back", "backup" }), false);
    observe(kStorageWrite, operation_match(text, { "storage", "writeback", "write_back", "write" }), fact.role == "writeback_io_observed", false);
    observe(kStoragePrefetchRead, operation_match(text, { "prefetch", "storage", "read" }), fact.role == "prefetch_io_observed", false);
    observe(kPrefetchApplyHost,
            operation_match(text, { "prefetch", "host", "progress" }),
            operation_match(text, { "prefetch_progress", "prefetch_intent" }),
            false);
    observe(kDeviceLoadback, operation_match(text, { "loadback", "load_back", "device" }), operation_match(text, { "insert_result", "capacity" }), false);
    observe(kHostCleanupReleaseDrain,
            operation_match(text, { "cleanup", "release", "drain" }),
            operation_match(text, { "lock_scope", "request_admission" }),
            false);
    observe(kWritebackAckOrDrain,
            operation_match(text, { "ack", "writeback", "drain" }),
            operation_match(text, { "writeback_enqueue", "writeback_io" }),
            false);
    observe(kPrefetchWaitOrTimeout,
            operation_match(text, { "wait", "timeout", "prefetch" }),
            operation_match(text, { "prefetch_progress", "prefetch_decision" }),
            false);
    observe(kSchedulerRadixCpuWork,
            operation_match(text, { "match_prefix", "prepare_for_extend", "_prefetch_kvcache", "radix" }),
            fact.fact_class == "workload_identity",
            fact.fact_class == "workload_identity");
}

void append_samples(EvidenceNodes & target, const EvidenceNodes & source) {
    target.count += source.count;
    target.duration_ns += source.duration_ns;
    for (size_t node_id : source.samples) {
        if (target.samples.size() >= kSampleLimit) break;
        target.samples.push_back(node_id);
    }
}

void merge_operation_evidence(std::vector<OperationEvidence> & target, const std::vector<OperationEvidence> & source) {
    for (size_t i = 0; i < target.size() && i < source.size(); ++i) {
        append_samples(target[i].direct_runtime_nodes, source[i].direct_runtime_nodes);
        append_samples(target[i].control_fact_nodes, source[i].control_fact_nodes);
        append_samples(target[i].state_fact_nodes, source[i].state_fact_nodes);
    }
}

void add_sync_coverage(SyncCoverageStats & target, const SyncCoverageStats & source) {
    target.event_wait_nodes += source.event_wait_nodes;
    target.stream_sync_nodes += source.stream_sync_nodes;
    target.event_sync_nodes += source.event_sync_nodes;
    target.device_sync_nodes += source.device_sync_nodes;
    target.notify_wait_nodes += source.notify_wait_nodes;
}

template <typename Map>
void add_count_map(Map & target, const Map & source) {
    for (const auto & item : source) target[item.first] += item.second;
}

void add_lane_stats(std::map<std::string, LaneStats> & target, const std::map<std::string, LaneStats> & source) {
    for (const auto & item : source) {
        auto & row = target[item.first];
        row.node_count += item.second.node_count;
        row.duration_ns += item.second.duration_ns;
    }
}

void add_lane_summary(LaneSummaryStats & target, const LaneSummaryStats & source) {
    add_lane_stats(target.cpu, source.cpu);
    add_lane_stats(target.device, source.device);
    add_lane_stats(target.hccl, source.hccl);
}

Json node_samples(const core::DagGraph & graph, const std::vector<size_t> & node_ids) {
    Json result = Json::array();
    for (size_t node_id : node_ids) result.push_back(compact_node_json(graph, node_id));
    return result;
}

Json operation_json(const core::DagGraph & graph, const OperationEvidence & evidence) {
    std::string visibility = "invisible";
    std::string duration_source = "none";
    if (evidence.direct_runtime_nodes.count > 0) {
        visibility = "visible";
        duration_source = "runtime_node";
    }
    else if (evidence.control_fact_nodes.count > 0 || evidence.state_fact_nodes.count > 0) {
        visibility = "partially_visible";
        duration_source = evidence.control_fact_nodes.count == 0 ? "state_fact_only" : "control_fact";
    }
    Json blockers = Json::array();
    if (visibility == "invisible") blockers.push_back("operation_invisible");
    if (duration_source == "none" || duration_source == "state_fact_only" || duration_source == "control_fact") blockers.push_back("duration_source_missing");
    return Json{
        {          "visibility",                                                                              visibility                                },
        {            "evidence",
         Json{
         { "direct_runtime_node_count", evidence.direct_runtime_nodes.count },
         { "direct_runtime_duration_ns", evidence.direct_runtime_nodes.duration_ns },
         { "control_fact_node_count", evidence.control_fact_nodes.count },
         { "control_fact_duration_ns", evidence.control_fact_nodes.duration_ns },
         { "state_fact_node_count", evidence.state_fact_nodes.count },
         { "state_fact_duration_ns", evidence.state_fact_nodes.duration_ns },
         { "direct_runtime_node_samples", node_samples(graph, evidence.direct_runtime_nodes.samples) },
         { "control_fact_node_samples", node_samples(graph, evidence.control_fact_nodes.samples) },
         }                                                                                                                                              },
        {    "possible_anchors", evidence.direct_runtime_nodes.count == 0 ? Json::array({ "state_fact_or_timestamp" }) : Json::array({ "runtime_node" }) },
        {     "duration_source",                                                                                                         duration_source },
        { "patchable_candidate",                                                                                                 visibility == "visible" },
        {            "blockers",                                                                                                                blockers },
    };
}

Json build_operation_visibility(const core::DagGraph & graph, const DagAnalysisScan & scan) {
    Json operations_json = Json::object();
    Json summary = {
        {           "visible_count", 0 },
        { "partially_visible_count", 0 },
        {         "invisible_count", 0 },
        {           "unknown_count", 0 },
    };
    for (const auto & evidence : scan.operations) {
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

DagAnalysisScan scan_node_range(const core::DagGraph & graph, size_t begin, size_t end) {
    DagAnalysisScan scan;
    scan.runtime_ts.reserve(end > begin ? end - begin : 0);
    scan.operations = make_operation_evidence();

    for (size_t node_id = begin; node_id < end; ++node_id) {
        const auto & node = graph.node(node_id);
        const auto & event = graph.event_for_node(node.id);
        if (node.is_cpu) scan.cpu_lanes.insert(node.lane_key);
        else scan.device_lanes.insert(node.lane_key);

        add_lane_summary_node(scan.lane_summary, node, event);
        scan.by_source[event_source(event)]++;
        scan.by_domain[event_domain(event)]++;
        scan.by_category[event.cat.empty() ? "unknown" : event.cat]++;

        if (event.name == "EVENT_WAIT") scan.sync_coverage.event_wait_nodes++;
        else if (event.name == "AscendCL@aclrtSynchronizeStream" || event.name == "AscendCL@aclrtSynchronizeStreamWithTimeout")
            scan.sync_coverage.stream_sync_nodes++;
        else if (event.name == "AscendCL@aclrtSynchronizeEvent" || event.name == "AscendCL@aclrtSynchronizeEventWithTimeout")
            scan.sync_coverage.event_sync_nodes++;
        else if (event.name == "AscendCL@aclrtSynchronizeDevice" || event.name == "AscendCL@aclrtSynchronizeDeviceWithTimeout")
            scan.sync_coverage.device_sync_nodes++;
        else if (event.name == "NOTIFY_WAIT") scan.sync_coverage.notify_wait_nodes++;

        if (raw_contains_key_hint(event, "correlation_id")) {
            auto value = event.arg("correlation_id");
            if (!value.empty()) scan.correlation_groups.insert(value);
        }
        if (raw_contains_key_hint(event, "connection_id")) {
            auto value = event.arg("connection_id");
            if (!value.empty()) scan.connection_groups.insert(value);
        }

        if (is_runtime_node(event)) scan.runtime_ts.push_back(event.ts);
        auto anchor_fact = parse_relevant_fact(event);
        if (anchor_fact.present && anchor_fact.fact_class == "workload_identity" && role_tracked(anchor_fact.role))
            scan.anchor_facts.push_back(AnchorFact{ .role = anchor_fact.role, .timestamp_ns = event.ts });

        observe_operation_event(scan.operations, node, event);
    }
    return scan;
}

void merge_scan(DagAnalysisScan & target, const DagAnalysisScan & source) {
    add_count_map(target.by_source, source.by_source);
    add_count_map(target.by_domain, source.by_domain);
    add_count_map(target.by_category, source.by_category);
    add_sync_coverage(target.sync_coverage, source.sync_coverage);
    add_lane_summary(target.lane_summary, source.lane_summary);
    target.cpu_lanes.insert(source.cpu_lanes.begin(), source.cpu_lanes.end());
    target.device_lanes.insert(source.device_lanes.begin(), source.device_lanes.end());
    target.correlation_groups.insert(source.correlation_groups.begin(), source.correlation_groups.end());
    target.connection_groups.insert(source.connection_groups.begin(), source.connection_groups.end());
    target.runtime_ts.insert(target.runtime_ts.end(), source.runtime_ts.begin(), source.runtime_ts.end());
    target.anchor_facts.insert(target.anchor_facts.end(), source.anchor_facts.begin(), source.anchor_facts.end());
    merge_operation_evidence(target.operations, source.operations);
}

DagAnalysisScan scan_dag_analysis_inputs(const core::DagGraph & graph, const std::unordered_map<std::string, size_t> & edge_counts, size_t threads) {
    const size_t concurrency = std::max<size_t>(1, std::min(std::max<size_t>(1, threads), graph.node_count()));
    DagAnalysisScan scan;
    scan.sync_coverage.sync_edges = edge_counts.contains("sync") ? edge_counts.at("sync") : 0;
    scan.operations = make_operation_evidence();
    if (graph.node_count() == 0) return scan;

    if (concurrency == 1 || graph.node_count() < 128'000) {
        auto single = scan_node_range(graph, 0, graph.node_count());
        merge_scan(scan, single);
        std::ranges::sort(scan.runtime_ts);
        return scan;
    }

    std::vector<std::future<DagAnalysisScan>> futures;
    futures.reserve(concurrency);
    const size_t chunk = (graph.node_count() + concurrency - 1) / concurrency;
    for (size_t begin = 0; begin < graph.node_count(); begin += chunk) {
        const size_t end = std::min(graph.node_count(), begin + chunk);
        futures.push_back(std::async(std::launch::async, [&graph, begin, end] { return scan_node_range(graph, begin, end); }));
    }
    for (auto & future : futures) {
        auto partial = future.get();
        merge_scan(scan, partial);
    }
    std::ranges::sort(scan.runtime_ts);
    return scan;
}

Json build_cycle_witness(const core::DagGraph & graph, const std::string & error_message) {
    auto witness = find_cycle_witness(graph);
    std::unordered_map<std::string, size_t> cycle_edge_counts;
    for (const auto & edge : witness.cycle_edges) cycle_edge_counts[edge_kind_name(edge.kind)]++;

    Json cycle_nodes = Json::array();
    const auto node_limit = std::min(witness.cycle_nodes.size(), kCycleWitnessNodeLimit);
    for (size_t i = 0; i < node_limit; ++i) cycle_nodes.push_back(cycle_node_json(graph, witness.cycle_nodes[i]));

    Json cycle_edges = Json::array();
    const auto edge_limit = std::min(witness.cycle_edges.size(), kCycleWitnessNodeLimit);
    for (size_t i = 0; i < edge_limit; ++i) cycle_edges.push_back(edge_ref_json(graph, witness.cycle_edges[i]));

    Json remaining_sample = Json::array();
    for (size_t node_id : witness.remaining_node_sample) remaining_sample.push_back(cycle_node_json(graph, node_id));

    Json blockers = Json::array();
    if (witness.remaining_node_count > 0) blockers.push_back("dag_cycle_detected");
    if (witness.cycle_nodes.empty() && witness.remaining_node_count > 0) blockers.push_back("cycle_witness_not_found");

    return Json{
        {                  "schema",       "markov.trace_graph.dag_cycle_witness.v1" },
        {          "cycle_detected",                witness.remaining_node_count > 0 },
        {           "error_message",                                  error_message },
        {              "node_count",                            graph.node_count() },
        {              "edge_count",                            graph.edge_count() },
        {        "processed_nodes",                       witness.processed_nodes },
        {   "remaining_node_count",                   witness.remaining_node_count },
        {       "cycle_node_count",                       witness.cycle_nodes.size() },
        {       "cycle_edge_count",                       witness.cycle_edges.size() },
        {          "cycle_nodes",                                      cycle_nodes },
        {          "cycle_edges",                                      cycle_edges },
        { "cycle_nodes_truncated",       witness.cycle_nodes.size() > node_limit },
        { "cycle_edges_truncated",       witness.cycle_edges.size() > edge_limit },
        {  "cycle_edge_counts_by_kind",          unordered_count_map_json(cycle_edge_counts) },
        { "remaining_node_sample",                              remaining_sample },
        {             "blockers",                                        blockers },
    };
}

} // namespace dag_analysis_detail

using dag_analysis_detail::build_anchor_coverage;
using dag_analysis_detail::build_cycle_witness;
using dag_analysis_detail::build_dag_analysis;
using dag_analysis_detail::build_dag_quality;
using dag_analysis_detail::build_operation_visibility;
using dag_analysis_detail::elapsed_ms;
using dag_analysis_detail::scan_dag_analysis_inputs;

DagAnalysisArtifacts build_dag_analysis_artifacts(const core::DagGraph & graph, size_t threads) {
    DagAnalysisArtifacts artifacts;
    auto total_start = std::chrono::steady_clock::now();

    auto start = std::chrono::steady_clock::now();
    const auto edge_counts = graph.edge_counts_by_kind();
    auto scan = scan_dag_analysis_inputs(graph, edge_counts, threads);
    auto end = std::chrono::steady_clock::now();
    artifacts.timings_ms["dag_analysis_shared_scan_ms"] = elapsed_ms(start, end);

    start = std::chrono::steady_clock::now();
    artifacts.dag_quality_json = build_dag_quality(graph, edge_counts, scan).dump(2);
    end = std::chrono::steady_clock::now();
    artifacts.timings_ms["dag_quality_ms"] = elapsed_ms(start, end);

    start = std::chrono::steady_clock::now();
    artifacts.dag_analysis_json = build_dag_analysis(graph, edge_counts, scan).dump(2);
    end = std::chrono::steady_clock::now();
    artifacts.timings_ms["dag_analysis_ms"] = elapsed_ms(start, end);

    start = std::chrono::steady_clock::now();
    artifacts.dag_anchor_coverage_json = build_anchor_coverage(scan).dump(2);
    end = std::chrono::steady_clock::now();
    artifacts.timings_ms["dag_anchor_coverage_ms"] = elapsed_ms(start, end);

    start = std::chrono::steady_clock::now();
    artifacts.dag_operation_visibility_json = build_operation_visibility(graph, scan).dump(2);
    end = std::chrono::steady_clock::now();
    artifacts.timings_ms["dag_operation_visibility_ms"] = elapsed_ms(start, end);

    auto total_end = std::chrono::steady_clock::now();
    artifacts.timings_ms["dag_analysis_artifacts_wall_ms"] = elapsed_ms(total_start, total_end);
    return artifacts;
}

std::string build_cycle_witness_json(const core::DagGraph & graph, const std::string & error_message) {
    return build_cycle_witness(graph, error_message).dump(2);
}

} // namespace markov::trace_graph::modules::dag_analysis
