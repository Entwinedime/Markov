/**
 * @file
 * @brief Builds a TraceGraph DAG from Chrome trace duration events.
 *
 * Construction extracts only trace-supported execution order, submission chains,
 * and synchronization. What-if modules and HiCache policy run after the base DAG.
 */
#include "markov/trace_graph/core/dag_builder.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#ifdef DEBUG
#include <chrono>
#endif
#include <future>
#include <numeric>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace markov::trace_graph::core {

namespace dag_builder_detail {

/**
 * @brief Recognizes HCCL naming conventions emitted by supported trace sources.
 *
 * This classification affects cross-rank merge. Keep the accepted names narrow and
 * trace-specific rather than adding unrelated business terms.
 */
bool contains_any_hccl_name(const std::string & name) { return name.contains("hcom") || name.contains("HCCL") || name.contains("hccl"); }

/**
 * @brief Returns the submit timestamp used to find stream-sync predecessors.
 *
 * `submit_ts` is inferred from the CPU anchor preceding the first device node in a
 * correlation or connection chain. Without an anchor, event start time is used so
 * only device work known to have started can enter the synchronization frontier.
 */
uint64_t event_submit_ts(const DagGraph & graph, size_t node_id) {
    const auto & node = graph.node(node_id);
    return node.submit_ts > 0 ? node.submit_ts : graph.event_for_node(node_id).ts;
}

bool is_submit_anchor_event(const TraceEvent & event) {
    if (event.name.starts_with("Enqueue@")) return true;
    if (event.name == "Node@launch") return true;
    if (event.cat == "enqueue") return true;
    if (event.name.starts_with("AscendCL@aclrtLaunch")) return true;
    if (event.name.starts_with("AscendCL@aclrtMemcpyAsync")) return true;
    if (event.name == "AscendCL@aclrtRecordEvent" || event.name == "AscendCL@aclrtWaitEvent") return true;
    return false;
}

/**
 * @brief Extracts a confirmed event ID from an Ascend CPU wrapper.
 *
 * Wrappers use both `Event Id` and `event_id`. Missing IDs cannot form record/wait
 * edges because grouping empty identities would create false dependencies.
 */
std::optional<std::string> event_id_from_cpu_record(const TraceEvent & event) {
    auto value = event.arg("Event Id");
    if (!value.empty()) return value;
    value = event.arg("event_id");
    if (!value.empty()) return value;
    return std::nullopt;
}

uint64_t node_end_ts(const TraceEvent & event) { return checked_add_u64(event.ts, event.dur, "trace event end timestamp overflow"); }

bool is_usable_lane_value(const std::string & value) { return !value.empty() && value != "-1"; }

bool is_stream_sync_event(const std::string & name) {
    return name == "AscendCL@aclrtSynchronizeStream" || name == "AscendCL@aclrtSynchronizeStreamWithTimeout";
}

bool is_event_sync_event(const std::string & name) { return name == "AscendCL@aclrtSynchronizeEvent" || name == "AscendCL@aclrtSynchronizeEventWithTimeout"; }

bool is_device_sync_event(const std::string & name) {
    return name == "AscendCL@aclrtSynchronizeDevice" || name == "AscendCL@aclrtSynchronizeDeviceWithTimeout";
}

bool raw_contains_key_hint(const TraceEvent & event, std::string_view key) { return event.args_json_view().find(key) != std::string_view::npos; }

bool is_hicache_fact_event(const TraceEvent & event) {
    return event.source_channel == TraceSourceChannel::PythonProbe && event.has_arg_key_hint("fact");
}

struct ExecutionAndFactEvents {
    std::vector<TraceEvent> executable_events;
    std::vector<TraceEvent> hicache_fact_events;
};

ExecutionAndFactEvents split_hicache_fact_events(std::vector<TraceEvent> events) {
    ExecutionAndFactEvents split;
    split.executable_events.reserve(events.size());
    for (auto & event : events) {
        if (is_hicache_fact_event(event)) split.hicache_fact_events.push_back(std::move(event));
        else split.executable_events.push_back(std::move(event));
    }
    return split;
}

struct EventLaneIdentity {
    bool is_device = false;
    std::string lane = "CPU_MERGED";
    std::optional<std::string> stream_id;
    std::optional<std::string> alternate_stream_id;
    std::optional<std::string> physical_stream_id;
};

std::optional<std::string> raw_hinted_arg(const TraceEvent & event, std::string_view key) {
    if (!raw_contains_key_hint(event, key)) return std::nullopt;
    return event.find_arg(key);
}

/**
 * @brief Resolves device classification, ordering lane, and optional sync aliases once.
 *
 * `Physic Stream Id` is the strongest device evidence. Kernel and cpu_op events may
 * instead use `streamId`. Device ordering prefers top-level `tid`, then `streamId`,
 * alternate `stream id`, and finally the physical stream ID.
 */
EventLaneIdentity resolve_event_lane(const TraceEvent & event, bool collect_aliases) {
    EventLaneIdentity identity;
    identity.physical_stream_id = raw_hinted_arg(event, "Physic Stream Id");
    const bool stream_identifies_device = event.cat == "Kernel" || event.cat == "cpu_op";
    if (stream_identifies_device) identity.stream_id = raw_hinted_arg(event, "streamId");
    identity.is_device = identity.physical_stream_id.has_value() || (stream_identifies_device && identity.stream_id.has_value());
    if (!identity.is_device) return identity;

    if (collect_aliases || !is_usable_lane_value(event.tid)) {
        if (!identity.stream_id) identity.stream_id = raw_hinted_arg(event, "streamId");
        identity.alternate_stream_id = raw_hinted_arg(event, "stream id");
    }
    if (is_usable_lane_value(event.tid)) identity.lane = event.tid;
    else if (identity.stream_id && is_usable_lane_value(*identity.stream_id)) identity.lane = *identity.stream_id;
    else if (identity.alternate_stream_id && is_usable_lane_value(*identity.alternate_stream_id)) identity.lane = *identity.alternate_stream_id;
    else if (identity.physical_stream_id && is_usable_lane_value(*identity.physical_stream_id)) identity.lane = *identity.physical_stream_id;
    else identity.lane = "NPU_UNKNOWN";
    return identity;
}

} // namespace dag_builder_detail

using dag_builder_detail::contains_any_hccl_name;
using dag_builder_detail::event_id_from_cpu_record;
using dag_builder_detail::event_submit_ts;
using dag_builder_detail::is_device_sync_event;
using dag_builder_detail::is_event_sync_event;
using dag_builder_detail::is_stream_sync_event;
using dag_builder_detail::is_submit_anchor_event;
using dag_builder_detail::is_usable_lane_value;
using dag_builder_detail::node_end_ts;
using dag_builder_detail::raw_contains_key_hint;
using dag_builder_detail::resolve_event_lane;
using dag_builder_detail::split_hicache_fact_events;

namespace {

enum class BuildStep : std::uint8_t {
    Normalize,
    CreateNodes,
    Correlation,
    Sequential,
    EventWait,
    NotifyWait,
    ModelExecute,
    StreamSync,
    EventSync,
    DeviceSync,
    Finalize,
    RealE2E,
};

struct DirectBuildProfiler {
    template <typename Function> decltype(auto) value(BuildStep, Function && function) { return std::forward<Function>(function)(); }

    template <typename Function> void run(BuildStep, Function && function) { std::forward<Function>(function)(); }
};

#ifdef DEBUG
struct TimedBuildProfiler {
    DagBuilder::BuildTimings & timings;

    template <typename Function> auto value(BuildStep step, Function && function) {
        const auto start = std::chrono::steady_clock::now();
        auto result = std::forward<Function>(function)();
        record(step, start);
        return result;
    }

    template <typename Function> void run(BuildStep step, Function && function) {
        const auto start = std::chrono::steady_clock::now();
        std::forward<Function>(function)();
        record(step, start);
    }

private:
    void record(BuildStep step, std::chrono::steady_clock::time_point start) {
        const auto elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        switch (step) {
        case BuildStep::Normalize:
            timings.normalize_ms = elapsed;
            break;
        case BuildStep::CreateNodes:
            timings.create_nodes_ms = elapsed;
            break;
        case BuildStep::Correlation:
            timings.correlation_ms = elapsed;
            break;
        case BuildStep::Sequential:
            timings.sequential_ms = elapsed;
            break;
        case BuildStep::EventWait:
            timings.event_wait_ms = elapsed;
            break;
        case BuildStep::NotifyWait:
            timings.notify_wait_ms = elapsed;
            break;
        case BuildStep::ModelExecute:
            timings.model_execute_ms = elapsed;
            break;
        case BuildStep::StreamSync:
            timings.stream_sync_ms = elapsed;
            break;
        case BuildStep::EventSync:
            timings.event_sync_ms = elapsed;
            break;
        case BuildStep::DeviceSync:
            timings.device_sync_ms = elapsed;
            break;
        case BuildStep::Finalize:
            timings.finalize_ms = elapsed;
            break;
        case BuildStep::RealE2E:
            timings.real_e2e_ms = elapsed;
            break;
        }
    }
};
#endif

struct PendingEdge {
    size_t src = 0;
    size_t dst = 0;
    DagEdgeKind kind = DagEdgeKind::Sequential;
};

struct PendingSubmit {
    size_t node_id = 0;
    uint64_t submit_ts = 0;
#ifdef DEBUG
    std::string anchor_name;
    std::string anchor_source;
#endif
};

struct CorrelationGroupResult {
    std::string key;
    std::vector<PendingEdge> edges;
    std::vector<PendingSubmit> submits;
};

struct SubmitFrontierNode {
    size_t node_id = 0;
    uint64_t effective_submit_ts = 0;
};

template <typename Result, typename Entry, typename Fn> std::vector<Result> map_entries_parallel(const std::vector<Entry> & entries, size_t threads, Fn fn) {
    std::vector<Result> results;
    results.reserve(entries.size());
    if (entries.empty()) return results;
    const size_t concurrency = std::max<size_t>(1, std::min(threads, entries.size()));
    if (concurrency == 1 || entries.size() < 64) {
        for (const auto & entry : entries) results.push_back(fn(entry));
        return results;
    }

    std::vector<std::future<std::vector<Result>>> futures;
    futures.reserve(concurrency);
    const size_t chunk = (entries.size() + concurrency - 1) / concurrency;
    for (size_t begin = 0; begin < entries.size(); begin += chunk) {
        const size_t end = std::min(entries.size(), begin + chunk);
        futures.push_back(std::async(std::launch::async, [&entries, begin, end, &fn] {
            std::vector<Result> local;
            local.reserve(end - begin);
            for (size_t i = begin; i < end; ++i) local.push_back(fn(entries[i]));
            return local;
        }));
    }
    for (auto & future : futures) {
        auto local = future.get();
        results.insert(results.end(), std::make_move_iterator(local.begin()), std::make_move_iterator(local.end()));
    }
    return results;
}

bool nodes_sorted_by_event_ts(const DagGraph & graph, const std::vector<size_t> & nodes) {
    return std::ranges::is_sorted(nodes, [&](size_t a, size_t b) {
        if (graph.event_for_node(a).ts != graph.event_for_node(b).ts) return graph.event_for_node(a).ts < graph.event_for_node(b).ts;
        return a < b;
    });
}

void sort_nodes_by_event_ts_if_needed(const DagGraph & graph, std::vector<size_t> & nodes) {
    if (nodes.size() <= 1 || nodes_sorted_by_event_ts(graph, nodes)) return;
    std::ranges::sort(nodes, [&](size_t a, size_t b) {
        if (graph.event_for_node(a).ts != graph.event_for_node(b).ts) return graph.event_for_node(a).ts < graph.event_for_node(b).ts;
        return a < b;
    });
}

std::optional<std::pair<size_t, std::string>> submit_anchor_before_first_device(const DagGraph & graph, const std::vector<size_t> & nodes) {
    std::optional<size_t> fallback_cpu;
    std::optional<size_t> submit_like_cpu;
    for (size_t node_id : nodes) {
        const auto & node = graph.node(node_id);
        if (!node.is_cpu) break;
        fallback_cpu = node_id;
        if (is_submit_anchor_event(graph.event_for_node(node_id))) submit_like_cpu = node_id;
    }
    if (submit_like_cpu) return std::make_pair(*submit_like_cpu, std::string("submit_like"));
    if (fallback_cpu) return std::make_pair(*fallback_cpu, std::string("fallback_pre_device_cpu"));
    return std::nullopt;
}

void add_submit_metadata_for_device_suffix(const DagGraph & graph, const std::vector<size_t> & nodes, CorrelationGroupResult & result) {
    auto anchor = submit_anchor_before_first_device(graph, nodes);
    if (!anchor) return;
    const auto anchor_node = anchor->first;
    const auto & anchor_event = graph.event_for_node(anchor_node);
    for (size_t node_id : nodes) {
        if (graph.node(node_id).is_cpu) continue;
        result.submits.push_back(PendingSubmit{
            .node_id = node_id,
            .submit_ts = anchor_event.ts,
#ifdef DEBUG
            .anchor_name = anchor_event.name,
            .anchor_source = anchor->second,
#endif
        });
    }
}

std::unordered_map<size_t, std::vector<SubmitFrontierNode>> build_submit_frontiers(const DagGraph & graph,
                                                                                   const std::unordered_map<size_t, std::vector<size_t>> & lane_to_nodes) {
    std::unordered_map<size_t, std::vector<SubmitFrontierNode>> frontiers;
    frontiers.reserve(lane_to_nodes.size());
    for (const auto & item : lane_to_nodes) {
        if (item.second.empty() || graph.node(item.second.front()).is_cpu) continue;
        uint64_t effective_submit_ts = 0;
        auto & frontier = frontiers[item.first];
        frontier.reserve(item.second.size());
        for (size_t node_id : item.second) {
            const auto submit_ts = event_submit_ts(graph, node_id);
            effective_submit_ts = std::max(effective_submit_ts, submit_ts);
            frontier.push_back(SubmitFrontierNode{ .node_id = node_id, .effective_submit_ts = effective_submit_ts });
        }
    }
    return frontiers;
}

std::optional<size_t> find_submitted_frontier_node(const std::vector<SubmitFrontierNode> & frontier, uint64_t sync_ts) {
    auto bound = std::lower_bound(frontier.begin(), frontier.end(), sync_ts, [](const SubmitFrontierNode & node, uint64_t value) {
        return node.effective_submit_ts < value;
    });
    if (bound == frontier.begin()) return std::nullopt;
    --bound;
    return bound->node_id;
}

using NodeGroups = std::unordered_map<std::string, std::vector<size_t>>;
using LaneNodes = std::unordered_map<size_t, std::vector<size_t>>;
using StreamLaneMap = std::unordered_map<std::string, size_t>;
using CausalityEntry = std::pair<std::string, std::vector<size_t> *>;
using SubmitFrontiers = std::unordered_map<size_t, std::vector<SubmitFrontierNode>>;

/** @brief Mutable lane-order inputs and the derived notify-wait index. */
struct LaneOrderBuffers {
    std::vector<size_t> & nodes;
    std::vector<size_t> & notify_wait_nodes;
};

/** @brief Indices updated while binding one device event record. */
struct EventRecordBindings {
    const NodeGroups & connection_to_nodes;
    StreamLaneMap & raw_stream_to_lane;
    StreamLaneMap & stream_alias_to_lane;
    NodeGroups & event_id_to_nodes;
};

/** @brief Read-only identity indices used to resolve one event wait. */
struct EventWaitBindings {
    const NodeGroups & connection_to_nodes;
    const NodeGroups & event_id_to_nodes;
};

std::vector<CausalityEntry> multi_node_entries(NodeGroups & groups) {
    std::vector<CausalityEntry> entries;
    entries.reserve(groups.size());
    for (auto & [key, nodes] : groups) {
        if (nodes.size() > 1) entries.emplace_back(key, &nodes);
    }
    return entries;
}

void append_causality_chain(const std::vector<size_t> & nodes, CorrelationGroupResult & result) {
    result.edges.reserve(nodes.size() - 1);
    for (size_t index = 1; index < nodes.size(); ++index) {
        result.edges.push_back(PendingEdge{ .src = nodes[index - 1], .dst = nodes[index], .kind = DagEdgeKind::Correlation });
    }
}

CorrelationGroupResult build_correlation_group(const DagGraph & graph, const CausalityEntry & entry) {
    auto & nodes = *entry.second;
    sort_nodes_by_event_ts_if_needed(graph, nodes);
    CorrelationGroupResult result{ .key = entry.first, .edges = {}, .submits = {} };
    result.submits.reserve(nodes.size() - 1);
    add_submit_metadata_for_device_suffix(graph, nodes, result);
    append_causality_chain(nodes, result);
    return result;
}

CorrelationGroupResult build_connection_group(const DagGraph & graph, const CausalityEntry & entry) {
    auto & nodes = *entry.second;
    sort_nodes_by_event_ts_if_needed(graph, nodes);
    CorrelationGroupResult result{ .key = entry.first, .edges = {}, .submits = {} };
    if (nodes.size() >= 3 && graph.event_for_node(nodes.front()).name != "Node@launch") return result;
    result.submits.reserve(nodes.size());
    add_submit_metadata_for_device_suffix(graph, nodes, result);
    append_causality_chain(nodes, result);
    return result;
}

void apply_causality_results(DagGraph & graph, std::vector<CorrelationGroupResult> & results) {
    std::ranges::sort(results, [](const auto & left, const auto & right) { return left.key < right.key; });
    for (const auto & result : results) {
        for (const auto & submit : result.submits) {
            auto & node = graph.mutable_node(submit.node_id);
            if (node.submit_ts > 0 && node.submit_ts >= submit.submit_ts) continue;
            node.submit_ts = submit.submit_ts;
#ifdef DEBUG
            auto & event = graph.mutable_event_for_node(submit.node_id);
            event.set_arg("submit_anchor_name", submit.anchor_name);
            event.set_arg("submit_anchor_source", submit.anchor_source);
#endif
        }
        for (const auto & edge : result.edges) graph.add_edge(edge.src, edge.dst, edge.kind);
    }
}

void record_lane_edge_metadata(DagGraph & graph, size_t previous_node, size_t node_id, bool is_cpu, std::vector<size_t> & notify_wait_nodes) {
    const auto & previous = graph.event_for_node(previous_node);
    const auto & event = graph.event_for_node(node_id);
    if (is_cpu) {
        const auto previous_end = node_end_ts(previous);
        graph.mutable_node(previous_node).cpu_gap_after = event.ts > previous_end ? event.ts - previous_end : 0;
    }
    if (event.name == "NOTIFY_WAIT" && previous.name == "MODEL_EXECUTE") notify_wait_nodes.push_back(node_id);
    if (!is_cpu && contains_any_hccl_name(previous.name)) graph.mutable_node(previous_node).hccl_successor_node_id = node_id;
}

void add_lane_order_edges(DagGraph & graph, const LaneOrderBuffers & buffers) {
    sort_nodes_by_event_ts_if_needed(graph, buffers.nodes);
    if (buffers.nodes.empty()) return;
    const bool is_cpu = graph.node(buffers.nodes.front()).is_cpu;
    const auto edge_kind = is_cpu ? DagEdgeKind::Sequential : DagEdgeKind::Stream;
    for (size_t index = 1; index < buffers.nodes.size(); ++index) {
        const auto previous_node = buffers.nodes[index - 1];
        const auto node_id = buffers.nodes[index];
        graph.add_edge(previous_node, node_id, edge_kind);
        record_lane_edge_metadata(graph, previous_node, node_id, is_cpu, buffers.notify_wait_nodes);
    }
}

void bind_event_record(DagGraph & graph, size_t record_node, const EventRecordBindings & bindings) {
    const auto & record_event = graph.event_for_node(record_node);
    const auto connection = bindings.connection_to_nodes.find(record_event.arg("connection_id"));
    if (connection == bindings.connection_to_nodes.end() || connection->second.empty()) return;
    const auto & cpu_event = graph.event_for_node(connection->second.front());
    const auto raw_stream = cpu_event.arg("Raw Stream");
    if (!raw_stream.empty()) {
        const auto lane = graph.node(record_node).lane_id;
        bindings.raw_stream_to_lane[raw_stream] = lane;
        bindings.stream_alias_to_lane[raw_stream] = lane;
    }
    const auto event_id = event_id_from_cpu_record(cpu_event);
    if (event_id) bindings.event_id_to_nodes[*event_id].push_back(record_node);
}

void sort_event_records(const DagGraph & graph, NodeGroups & event_id_to_nodes) {
    for (auto & [event_id, nodes] : event_id_to_nodes) {
        (void)event_id;
        std::ranges::sort(nodes, [&](size_t left, size_t right) { return graph.event_for_node(left).ts < graph.event_for_node(right).ts; });
    }
}

std::optional<size_t> latest_cross_lane_record(const DagGraph & graph, size_t wait_node, const std::vector<size_t> & records) {
    const auto & wait_event = graph.event_for_node(wait_node);
    auto bound = records.end();
    if (wait_event.dur == 0) {
        const auto bound_value = wait_event.ts > 0 ? wait_event.ts - 1 : 0;
        bound = std::upper_bound(records.begin(), records.end(), bound_value, [&](uint64_t value, size_t node_id) {
            return value < graph.event_for_node(node_id).ts;
        });
    }
    else {
        const auto bound_value = static_cast<double>(wait_event.ts) + static_cast<double>(wait_event.dur) - 0.1;
        bound = std::upper_bound(records.begin(), records.end(), bound_value, [&](double value, size_t node_id) {
            return value < static_cast<double>(graph.event_for_node(node_id).ts);
        });
    }
    if (bound == records.begin()) return std::nullopt;
    --bound;
    const auto wait_lane = graph.node(wait_node).lane_id;
    while (bound != records.begin() && wait_lane == graph.node(*bound).lane_id) --bound;
    if (wait_lane == graph.node(*bound).lane_id) return std::nullopt;
    return *bound;
}

std::optional<std::string> wait_event_id(const DagGraph & graph, size_t wait_node, const NodeGroups & connection_to_nodes) {
    const auto & wait_event = graph.event_for_node(wait_node);
    const auto connection = connection_to_nodes.find(wait_event.arg("connection_id"));
    if (connection == connection_to_nodes.end() || connection->second.empty()) return std::nullopt;
    return event_id_from_cpu_record(graph.event_for_node(connection->second.front()));
}

void add_event_wait_dependency(DagGraph & graph, size_t wait_node, const EventWaitBindings & bindings) {
    const auto event_id = wait_event_id(graph, wait_node, bindings.connection_to_nodes);
    if (!event_id) return;
    const auto records = bindings.event_id_to_nodes.find(*event_id);
    if (records == bindings.event_id_to_nodes.end()) return;
    const auto record = latest_cross_lane_record(graph, wait_node, records->second);
    if (record) graph.add_edge(*record, wait_node, DagEdgeKind::Sync);
}

uint64_t earliest_notify_end(const DagGraph & graph, uint64_t model_start, const std::vector<size_t> & notify_wait_nodes) {
    uint64_t notify_end = model_start;
    for (const auto wait_node : notify_wait_nodes) {
        const auto & wait_event = graph.event_for_node(wait_node);
        if (wait_event.ts < model_start) continue;
        const auto end = node_end_ts(wait_event);
        if (notify_end == model_start || end < notify_end) notify_end = end;
    }
    return notify_end;
}

std::optional<size_t> first_node_in_window(const DagGraph & graph, const std::vector<size_t> & nodes, uint64_t begin, uint64_t end) {
    for (const auto node_id : nodes) {
        const auto timestamp = graph.event_for_node(node_id).ts;
        if (timestamp >= begin && timestamp <= end) return node_id;
    }
    return std::nullopt;
}

std::optional<size_t> resolve_stream_lane(const DagGraph & graph, const LaneNodes & lane_to_nodes, const StreamLaneMap & raw_stream_to_lane,
                                          const StreamLaneMap & stream_alias_to_lane, const std::string & stream_id) {
    std::optional<size_t> lane;
    if (const auto raw = raw_stream_to_lane.find(stream_id); raw != raw_stream_to_lane.end()) lane = raw->second;
    if (!lane) {
        if (const auto alias = stream_alias_to_lane.find(stream_id); alias != stream_alias_to_lane.end()) lane = alias->second;
    }
    if (!lane) lane = graph.find_lane_id(stream_id);
    if (lane && lane_to_nodes.contains(*lane)) return lane;
    return std::nullopt;
}

std::vector<size_t> stream_sync_target_lanes(const DagGraph & graph, const TraceEvent & sync_event, const LaneNodes & lane_to_nodes,
                                             const StreamLaneMap & raw_stream_to_lane, const StreamLaneMap & stream_alias_to_lane) {
    std::vector<size_t> lanes;
    std::unordered_set<size_t> seen;
    bool has_stream_evidence = false;
    constexpr std::string_view keys[] = { "Raw Stream", "streamId", "stream id", "Physic Stream Id" };
    for (const auto key : keys) {
        const auto stream_id = sync_event.arg(key);
        if (!is_usable_lane_value(stream_id)) continue;
        has_stream_evidence = true;
        const auto lane = resolve_stream_lane(graph, lane_to_nodes, raw_stream_to_lane, stream_alias_to_lane, stream_id);
        if (lane && seen.insert(*lane).second) lanes.push_back(*lane);
    }
    if (has_stream_evidence) return lanes;
    for (const auto & [lane_id, nodes] : lane_to_nodes) {
        if (!nodes.empty() && !graph.node(nodes.front()).is_cpu) lanes.push_back(lane_id);
    }
    return lanes;
}

void add_stream_sync_dependency(DagGraph & graph, size_t sync_node, const LaneNodes & lane_to_nodes, const StreamLaneMap & raw_stream_to_lane,
                                const StreamLaneMap & stream_alias_to_lane, const SubmitFrontiers & submit_frontiers) {
    const auto & sync_event = graph.event_for_node(sync_node);
    const auto target_lanes = stream_sync_target_lanes(graph, sync_event, lane_to_nodes, raw_stream_to_lane, stream_alias_to_lane);
    for (const auto lane : target_lanes) {
        const auto frontier = submit_frontiers.find(lane);
        if (frontier == submit_frontiers.end()) continue;
        const auto submitted_node = find_submitted_frontier_node(frontier->second, sync_event.ts);
        if (submitted_node) graph.add_edge(*submitted_node, sync_node, DagEdgeKind::Sync);
    }
}

} // namespace

DagBuilder::DagBuilder(size_t threads) : threads_(std::max<size_t>(1, threads)) {}

DagGraph DagBuilder::build(std::vector<TraceEvent> events, int gpu_id) const {
    DirectBuildProfiler profiler;
    return build_impl(std::move(events), gpu_id, profiler);
}

template <typename Profiler> DagGraph DagBuilder::build_impl(std::vector<TraceEvent> events, int gpu_id, Profiler & profiler) const {
    auto parsed_count = events.size();
    auto split = split_hicache_fact_events(std::move(events));
    auto normalized = profiler.value(BuildStep::Normalize, [&] { return normalize_events(std::move(split.executable_events)); });
    DagGraph graph(std::move(normalized), gpu_id);
    graph.set_parsed_record_count(parsed_count);
    graph.set_hicache_fact_events(std::move(split.hicache_fact_events));
    graph.reserve(DagGraphCapacity{ .nodes = graph.events().size(), .edges = 0 });

    auto index = profiler.value(BuildStep::CreateNodes, [&] { return create_nodes(graph); });
    graph.reserve(DagGraphCapacity{ .nodes = graph.node_count(), .edges = estimate_edge_capacity(graph, index) });
    profiler.run(BuildStep::Correlation, [&] { add_correlation_edges(graph, index); });
    profiler.run(BuildStep::Sequential, [&] { add_sequential_edges(graph, index); });
    profiler.run(BuildStep::EventWait, [&] { add_event_wait_edges(graph, index); });
    profiler.run(BuildStep::NotifyWait, [&] { add_notify_wait_edges(graph, index); });
    profiler.run(BuildStep::ModelExecute, [&] { add_model_execute_edges(graph, index); });
    profiler.run(BuildStep::StreamSync, [&] { add_stream_sync_edges(graph, index); });
    profiler.run(BuildStep::EventSync, [&] { add_event_sync_edges(graph, index); });
    profiler.run(BuildStep::DeviceSync, [&] { add_device_sync_edges(graph, index); });
    profiler.run(BuildStep::Finalize, [&] { finalize_sync_nodes(graph, index); });
#ifdef DEBUG
    profiler.run(BuildStep::RealE2E, [&] { set_real_e2e_time(graph); });
#endif
    return graph;
}

#ifdef DEBUG
DagGraph DagBuilder::build_with_timings(std::vector<TraceEvent> events, int gpu_id, BuildTimings & timings) const {
    TimedBuildProfiler profiler{ timings };
    return build_impl(std::move(events), gpu_id, profiler);
}

void DagBuilder::set_real_e2e_time(DagGraph & graph) {
    uint64_t real_min = 0;
    uint64_t real_max = 0;
    bool has_real_time = false;
    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        if (!has_real_time || event.ts < real_min) real_min = event.ts;
        const auto event_end = checked_add_u64(event.ts, event.dur, "trace timestamp overflow while measuring observed E2E");
        if (event_end > real_max) real_max = event_end;
        has_real_time = true;
    }
    graph.set_real_e2e_time(has_real_time && real_max > real_min ? real_max - real_min : 0);
}
#endif

class DagBuilder::EventNormalizer {
public:
    explicit EventNormalizer(std::vector<TraceEvent> events) : events_(std::move(events)), dropped_(events_.size(), false) {}

    [[nodiscard]] std::vector<TraceEvent> run() {
        build_event_order();
        coalesce_runtime_boundaries();
        auto deduped = move_retained_events();
        sort_equal_timestamp_groups(deduped);
        return retain_cpu_leaves(std::move(deduped));
    }

private:
    struct LaneEvents {
        std::vector<size_t> indices;
        bool has_cpu = false;
    };

    struct LeafSelection {
        explicit LeafSelection(size_t event_count) : is_leaf(event_count, true), discarded(event_count, false), is_enqueue_node(event_count, false) {}

        std::vector<bool> is_leaf;
        std::vector<bool> discarded;
        std::vector<bool> is_enqueue_node;
    };

    [[nodiscard]] bool logical_index_less(size_t left, size_t right) const {
        const auto & lhs = events_[left];
        const auto & rhs = events_[right];
        if (lhs.ts != rhs.ts) return lhs.ts < rhs.ts;
        if (lhs.pid != rhs.pid) return lhs.pid < rhs.pid;
        if (lhs.tid != rhs.tid) return lhs.tid < rhs.tid;
        if (lhs.name != rhs.name) return lhs.name < rhs.name;
        if (lhs.index != rhs.index) return lhs.index < rhs.index;
        return left < right;
    }

    [[nodiscard]] size_t event_index_at(size_t position) const { return sorted_by_timestamp_ ? position : event_order_[position]; }

    void build_event_order() {
        sorted_by_timestamp_ = std::ranges::is_sorted(events_, [](const TraceEvent & left, const TraceEvent & right) { return left.ts < right.ts; });
        if (sorted_by_timestamp_) return;
        event_order_.resize(events_.size());
        std::iota(event_order_.begin(), event_order_.end(), size_t{ 0 });
        std::ranges::sort(event_order_, [this](size_t left, size_t right) { return logical_index_less(left, right); });
    }

    /**
     * @brief Coalesces torch and wrapper events that describe one runtime boundary.
     *
     * @warning The key intentionally contains only timestamp and duration for large-trace
     * performance. Distinct events with identical values can be merged; tightening the
     * key requires evidence from production trace collision rates.
     */
    void coalesce_runtime_boundaries() {
        for (size_t timestamp_begin = 0; timestamp_begin < events_.size();) {
            size_t timestamp_end = timestamp_begin + 1;
            const auto timestamp = events_[event_index_at(timestamp_begin)].ts;
            while (timestamp_end < events_.size() && events_[event_index_at(timestamp_end)].ts == timestamp) ++timestamp_end;
            if (timestamp_end - timestamp_begin > 1) coalesce_timestamp_group(timestamp_begin, timestamp_end);
            timestamp_begin = timestamp_end;
        }
    }

    void coalesce_timestamp_group(size_t begin, size_t end) {
        duration_order_.clear();
        duration_order_.reserve(end - begin);
        for (size_t position = begin; position < end; ++position) duration_order_.push_back(event_index_at(position));
        std::ranges::sort(duration_order_, [this](size_t left, size_t right) {
            if (events_[left].dur != events_[right].dur) return events_[left].dur < events_[right].dur;
            return logical_index_less(left, right);
        });

        for (size_t duration_begin = 0; duration_begin < duration_order_.size();) {
            size_t duration_end = duration_begin + 1;
            while (duration_end < duration_order_.size() && events_[duration_order_[duration_end]].dur == events_[duration_order_[duration_begin]].dur)
                ++duration_end;
            if (duration_end - duration_begin > 1) coalesce_duration_group(duration_begin, duration_end);
            duration_begin = duration_end;
        }
    }

    void coalesce_duration_group(size_t begin, size_t end) {
        size_t device_index = static_cast<size_t>(-1);
        for (size_t position = begin; position < end; ++position) {
            const auto index = duration_order_[position];
            if (events_[index].has_arg("Physic Stream Id")) {
                device_index = index;
                break;
            }
        }
        if (device_index == static_cast<size_t>(-1)) return;

        for (size_t position = begin; position < end; ++position) {
            const auto index = duration_order_[position];
            if (index == device_index) continue;
            events_[device_index].merge_args_from(events_[index]);
            if (events_[index].tid == "0") events_[device_index].name = events_[index].name;
            dropped_[index] = true;
        }
    }

    [[nodiscard]] std::vector<TraceEvent> move_retained_events() {
        std::vector<TraceEvent> retained;
        retained.reserve(events_.size());
        for (const auto position : std::views::iota(size_t{ 0 }, events_.size())) {
            const auto event_index = event_index_at(position);
            if (dropped_[event_index]) continue;
            retained.push_back(std::move(events_[event_index]));
        }
        return retained;
    }

    static void sort_equal_timestamp_groups(std::vector<TraceEvent> & events) {
        for (size_t begin = 0; begin < events.size();) {
            size_t end = begin + 1;
            while (end < events.size() && events[end].ts == events[begin].ts) ++end;
            if (end - begin > 1) {
                std::sort(events.begin() + static_cast<std::ptrdiff_t>(begin),
                          events.begin() + static_cast<std::ptrdiff_t>(end),
                          [](const TraceEvent & left, const TraceEvent & right) {
                              if (left.dur != right.dur) return left.dur > right.dur;
                              if (left.pid != right.pid) return left.pid < right.pid;
                              if (left.tid != right.tid) return left.tid < right.tid;
                              if (left.name != right.name) return left.name < right.name;
                              return left.index < right.index;
                          });
            }
            begin = end;
        }
    }

    /**
     * @brief Builds per-lane sequences and retains only leaves from nested CPU call trees.
     *
     * Device events never enter CPU-leaf filtering because kernels and copies are
     * executable graph nodes rather than nested CPU call frames. Semantic HiCache
     * facts are separated before normalization and do not enter this executable path.
     */
    [[nodiscard]] static std::vector<TraceEvent> retain_cpu_leaves(std::vector<TraceEvent> events) {
        auto lanes = build_lane_index(events);
        LeafSelection selection(events.size());
        std::unordered_set<std::string> seen_correlation_ids;
        for (auto & [lane, lane_events] : lanes) {
            (void)lane;
            if (!lane_events.has_cpu) continue;
            mark_lane_leaves(events, lane_events.indices, selection, seen_correlation_ids);
        }

        std::vector<TraceEvent> result;
        result.reserve(events.size());
        for (const auto index : std::views::iota(size_t{ 0 }, events.size())) {
            if (selection.is_leaf[index] && !selection.discarded[index])
                result.push_back(std::move(events[index]));
        }
        for (const auto index : std::views::iota(size_t{ 0 }, result.size())) result[index].index = index;
        return result;
    }

    [[nodiscard]] static std::unordered_map<std::string, LaneEvents> build_lane_index(const std::vector<TraceEvent> & events) {
        std::unordered_map<std::string, LaneEvents> lanes;
        for (const auto index : std::views::iota(size_t{ 0 }, events.size())) {
            auto identity = resolve_event_lane(events[index], false);
            auto & lane = lanes[std::move(identity.lane)];
            lane.indices.push_back(index);
            lane.has_cpu = lane.has_cpu || !identity.is_device;
        }
        return lanes;
    }

    [[nodiscard]] static std::string correlation_id(const TraceEvent & event) {
        return event.has_arg_key_hint("correlation_id") ? event.arg("correlation_id") : std::string{};
    }

    static void resolve_nested_parent(std::vector<TraceEvent> & events, size_t current_index, std::vector<size_t> & stack, LeafSelection & selection,
                                      const std::string & current_correlation_id) {
        auto & current = events[current_index];
        while (!stack.empty()) {
            const auto parent_index = stack.back();
            auto & parent = events[parent_index];
            const double current_end_threshold = static_cast<double>(current.ts) + static_cast<double>(current.dur) * 0.5;
            const double parent_end = static_cast<double>(parent.ts) + static_cast<double>(parent.dur);
            if (parent_end <= current_end_threshold) {
                stack.pop_back();
                continue;
            }
            if (parent.name == "Node@launch" || selection.is_enqueue_node[parent_index] || current.name.starts_with("Runtime@")) {
                selection.discarded[current_index] = true;
                return;
            }
            if (parent.name != "AscendCL@aclrtRecordEvent") selection.is_leaf[parent_index] = false;
            if (current_correlation_id.empty()) {
                const auto parent_correlation_id = correlation_id(parent);
                if (!parent_correlation_id.empty()) current.set_arg("correlation_id", parent_correlation_id);
            }
            return;
        }
    }

    /**
     * @brief Applies the nested CPU-frame heuristic to one timestamp-ordered lane.
     *
     * `Node@launch`, first-correlation enqueue nodes, runtime implementation details,
     * and `AscendCL@aclrtRecordEvent` retain the historical faithful-replay exceptions.
     */
    static void mark_lane_leaves(std::vector<TraceEvent> & events, const std::vector<size_t> & indices, LeafSelection & selection,
                                 std::unordered_set<std::string> & seen_correlation_ids) {
#ifdef DEBUG
        if (!std::ranges::is_sorted(indices, [&](size_t left, size_t right) {
                if (events[left].ts != events[right].ts) return events[left].ts < events[right].ts;
                return left < right;
            })) {
            throw std::logic_error("normalized lane order must preserve global timestamp order");
        }
#endif

        std::vector<size_t> stack;
        for (const size_t current_index : indices) {
            auto & current = events[current_index];
            const auto current_correlation_id = correlation_id(current);
            if (!current_correlation_id.empty() && seen_correlation_ids.insert(current_correlation_id).second) selection.is_enqueue_node[current_index] = true;
            resolve_nested_parent(events, current_index, stack, selection, current_correlation_id);
            if (!selection.discarded[current_index]) stack.push_back(current_index);
        }
    }

    std::vector<TraceEvent> events_;
    std::vector<bool> dropped_;
    std::vector<size_t> event_order_;
    std::vector<size_t> duration_order_;
    bool sorted_by_timestamp_ = true;
};

std::vector<TraceEvent> DagBuilder::normalize_events(std::vector<TraceEvent> events) { return EventNormalizer(std::move(events)).run(); }

class DagBuilder::NodeIndexer {
public:
    explicit NodeIndexer(DagGraph & graph) : graph_(graph) {
        index_.lane_to_nodes.reserve(256);
        index_.raw_stream_to_lane.reserve(256);
        index_.stream_alias_to_lane.reserve(512);
        index_.device_sync_nodes.reserve(16);
        index_.notify_record_nodes.reserve(64);
        index_.notify_wait_nodes.reserve(64);
        index_.model_execute_nodes.reserve(64);
    }

    [[nodiscard]] BuildIndex run() {
        for (const auto event_index : std::views::iota(size_t{ 0 }, graph_.events().size())) add_event(event_index);
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
        if (value && !value->empty()) groups[std::move(*value)].push_back(node_id);
    }

    /**
     * @brief Classifies synchronization anchors once while nodes are created.
     *
     * `EVENT_WAIT` moves one microsecond later while preserving its original end time,
     * removing an ambiguous equal-timestamp record/wait boundary.
     */
    void classify_special_event(TraceEvent & event, size_t node_id) {
        if (event.name == "EVENT_RECORD") index_.event_record_nodes.push_back(node_id);
        else if (event.name == "EVENT_WAIT") {
            event.ts += 1;
            if (event.dur > 0) event.dur -= 1;
            index_.event_wait_nodes.push_back(node_id);
        }
        else if (is_stream_sync_event(event.name)) index_.stream_sync_nodes.push_back(node_id);
        else if (is_event_sync_event(event.name)) index_.event_sync_nodes.push_back(node_id);
        else if (is_device_sync_event(event.name)) index_.device_sync_nodes.push_back(node_id);
        else if (event.name == "NOTIFY_RECORD") index_.notify_record_nodes.push_back(node_id);
        else if (event.name == "NOTIFY_WAIT") index_.notify_wait_candidate_count++;
        else if (event.name == "MODEL_EXECUTE") index_.model_execute_nodes.push_back(node_id);
    }

    DagGraph & graph_;
    BuildIndex index_;
};

DagBuilder::BuildIndex DagBuilder::create_nodes(DagGraph & graph) const { return NodeIndexer(graph).run(); }

size_t DagBuilder::estimate_edge_capacity(const DagGraph & graph, const BuildIndex & index) {
    size_t capacity = 0;
    auto add = [&](size_t value) {
        if (value > std::numeric_limits<size_t>::max() - capacity) throw std::length_error("base DAG edge-capacity overflow");
        capacity += value;
    };
    auto add_group_edges = [&](const auto & groups) {
        for (const auto & [key, nodes] : groups) {
            (void)key;
            if (nodes.size() > 1) add(nodes.size() - 1);
        }
    };

    size_t device_lane_count = 0;
    for (const auto & [lane_id, nodes] : index.lane_to_nodes) {
        (void)lane_id;
        if (nodes.empty()) continue;
        add(nodes.size() - 1);
        if (!graph.node(nodes.front()).is_cpu) device_lane_count++;
    }
    add_group_edges(index.correlation_to_nodes);
    add_group_edges(index.connection_to_nodes);
    add(index.event_wait_nodes.size());
    add(index.notify_wait_candidate_count);
    add(index.event_sync_nodes.size());

    const auto lane_fanout_nodes = checked_add_u64(checked_add_u64(static_cast<uint64_t>(index.stream_sync_nodes.size()),
                                                                   static_cast<uint64_t>(index.device_sync_nodes.size()),
                                                                   "base DAG sync-node count overflow"),
                                                   static_cast<uint64_t>(index.model_execute_nodes.size()),
                                                   "base DAG sync-node count overflow");
    const auto lane_fanout_edges = checked_multiply_u64(lane_fanout_nodes, static_cast<uint64_t>(device_lane_count), "base DAG lane-fanout overflow");
    if (lane_fanout_edges > std::numeric_limits<size_t>::max()) throw std::length_error("base DAG edge-capacity overflow");
    add(static_cast<size_t>(lane_fanout_edges));
    return capacity;
}

void DagBuilder::add_correlation_edges(DagGraph & graph, BuildIndex & index) const {
    /**
     * @brief Builds CPU-runtime to device-kernel submission chains by correlation ID.
     *
     * Torch profiler commonly shares this identity across launch, runtime, and kernel
     * events. Timestamp order within one identity defines the submission chain.
     */
    auto correlation_entries = multi_node_entries(index.correlation_to_nodes);
    auto correlation_results =
        map_entries_parallel<CorrelationGroupResult>(correlation_entries, threads_, [&](const auto & item) { return build_correlation_group(graph, item); });

    /**
     * @brief Builds CANN runtime-to-device chains by connection ID.
     *
     * Chains of three or more events are rejected when they do not begin with
     * `Node@launch`; such groups frequently combine unrelated runtime events.
     */
    auto connection_entries = multi_node_entries(index.connection_to_nodes);
    auto connection_results =
        map_entries_parallel<CorrelationGroupResult>(connection_entries, threads_, [&](const auto & item) { return build_connection_group(graph, item); });
    apply_causality_results(graph, correlation_results);
    apply_causality_results(graph, connection_results);
}

void DagBuilder::add_sequential_edges(DagGraph & graph, BuildIndex & index) const {
    /**
     * @brief Orders each execution lane by event timestamp.
     *
     * CPU lanes produce sequential edges and device lanes produce stream edges. Both
     * are hard dependencies; distinct kinds preserve diagnostic attribution.
     */
    for (auto & [lane_id, nodes] : index.lane_to_nodes) {
        (void)lane_id;
        add_lane_order_edges(graph, LaneOrderBuffers{ .nodes = nodes, .notify_wait_nodes = index.notify_wait_nodes });
    }
}

void DagBuilder::add_event_wait_edges(DagGraph & graph, BuildIndex & index) const {
    /**
     * @brief Phase one: associates CPU record wrappers with event and stream IDs.
     *
     * The fields are attached to matching device records for event-wait and stream-sync
     * dependency recovery.
     */
    for (const auto record_node : index.event_record_nodes) {
        bind_event_record(graph,
                          record_node,
                          EventRecordBindings{
                              .connection_to_nodes = index.connection_to_nodes,
                              .raw_stream_to_lane = index.raw_stream_to_lane,
                              .stream_alias_to_lane = index.stream_alias_to_lane,
                              .event_id_to_nodes = index.event_id_to_nodes,
                          });
    }
    sort_event_records(graph, index.event_id_to_nodes);

    /**
     * @brief Phase two: connects each wait to the latest matching event record.
     *
     * A cross-lane pair receives an explicit synchronization dependency.
     */
    for (const auto wait_node : index.event_wait_nodes) {
        add_event_wait_dependency(graph,
                                  wait_node,
                                  EventWaitBindings{
                                      .connection_to_nodes = index.connection_to_nodes,
                                      .event_id_to_nodes = index.event_id_to_nodes,
                                  });
    }
}

void DagBuilder::add_notify_wait_edges(DagGraph & graph, BuildIndex & index) const {
    /**
     * @brief Connects model-execution anchors represented by notify record/wait pairs.
     *
     * Each wait uses the latest record in the 200-microsecond window before wait end.
     */
    std::ranges::sort(index.notify_record_nodes, [&](size_t a, size_t b) { return graph.event_for_node(a).ts < graph.event_for_node(b).ts; });
    for (size_t wait_node : index.notify_wait_nodes) {
        auto wait_end = node_end_ts(graph.event_for_node(wait_node));
        auto bound_value = wait_end > 200 ? wait_end - 200 : 0;
        auto it = std::upper_bound(index.notify_record_nodes.begin(), index.notify_record_nodes.end(), bound_value, [&](uint64_t value, size_t node_id) {
            return value < graph.event_for_node(node_id).ts;
        });
        if (it == index.notify_record_nodes.begin()) continue;
        --it;
        graph.add_edge(*it, wait_node, DagEdgeKind::Sync);
    }
}

void DagBuilder::add_model_execute_edges(DagGraph & graph, BuildIndex & index) const {
    /**
     * @brief Conservatively anchors device lanes to `MODEL_EXECUTE`.
     *
     * Between model start and the earliest notify-wait end, the first node on each
     * device lane depends on `MODEL_EXECUTE`, preventing simulation from moving work
     * ahead of its CPU scheduling boundary.
     */
    for (size_t model_node : index.model_execute_nodes) {
        const auto model_start = graph.event_for_node(model_node).ts;
        const auto notify_end = earliest_notify_end(graph, model_start, index.notify_wait_nodes);
        if (notify_end <= model_start) continue;
        const auto model_lane = graph.node(model_node).lane_id;
        for (const auto & [lane_id, nodes] : index.lane_to_nodes) {
            if (nodes.empty() || graph.node(nodes.front()).is_cpu || lane_id == model_lane) continue;
            const auto first_node = first_node_in_window(graph, nodes, model_start, notify_end);
            if (first_node) graph.add_edge(model_node, *first_node, DagEdgeKind::Sync);
        }
    }
}

void DagBuilder::add_stream_sync_edges(DagGraph & graph, BuildIndex & index) const {
    /**
     * @brief Adds device-to-CPU dependencies for `aclrtSynchronizeStream`.
     *
     * The CPU waits for already submitted work on one stream. The latest submitted
     * device node on each resolved target lane therefore precedes the sync wrapper.
     */
    const auto submit_frontiers = build_submit_frontiers(graph, index.lane_to_nodes);
    for (const auto sync_node : index.stream_sync_nodes) {
        add_stream_sync_dependency(graph, sync_node, index.lane_to_nodes, index.raw_stream_to_lane, index.stream_alias_to_lane, submit_frontiers);
    }
}

void DagBuilder::add_event_sync_edges(DagGraph & graph, BuildIndex & index) const {
    /**
     * @brief Adds event-record dependencies for `aclrtSynchronizeEvent`.
     *
     * A low-level `EVENT_WAIT` may be absent, so event ID directly resolves the latest
     * matching record.
     */
    for (size_t sync_node : index.event_sync_nodes) {
        const auto & sync_event = graph.event_for_node(sync_node);
        auto event_id = event_id_from_cpu_record(sync_event);
        if (!event_id) {
            const auto connection_id = sync_event.arg("connection_id");
            auto conn_it = index.connection_to_nodes.find(connection_id);
            if (conn_it != index.connection_to_nodes.end() && !conn_it->second.empty())
                event_id = event_id_from_cpu_record(graph.event_for_node(conn_it->second.front()));
        }
        if (!event_id) continue;
        auto records_it = index.event_id_to_nodes.find(*event_id);
        if (records_it == index.event_id_to_nodes.end()) continue;

        auto & records = records_it->second;
        auto bound_value = node_end_ts(sync_event);
        auto bound = std::upper_bound(records.begin(), records.end(), bound_value, [&](uint64_t value, size_t node_id) {
            return value < graph.event_for_node(node_id).ts;
        });
        if (bound == records.begin()) continue;
        --bound;
        graph.add_edge(*bound, sync_node, DagEdgeKind::Sync);
    }
}

void DagBuilder::add_device_sync_edges(DagGraph & graph, BuildIndex & index) const {
    /**
     * @brief Adds all-lane dependencies for `aclrtSynchronizeDevice`.
     *
     * The CPU waits for all submitted streams on the current device. Within one rank,
     * the sync wrapper depends on the latest submitted node from every device lane.
     */
    const auto submit_frontiers = build_submit_frontiers(graph, index.lane_to_nodes);
    for (size_t sync_node : index.device_sync_nodes) {
        const auto & sync_event = graph.event_for_node(sync_node);
        for (const auto & item : index.lane_to_nodes) {
            if (item.second.empty() || graph.node(item.second.front()).is_cpu) continue;
            auto frontier_it = submit_frontiers.find(item.first);
            if (frontier_it == submit_frontiers.end()) continue;
            auto submitted_node = find_submitted_frontier_node(frontier_it->second, sync_event.ts);
            if (!submitted_node) continue;
            graph.add_edge(*submitted_node, sync_node, DagEdgeKind::Sync);
        }
    }
}

void DagBuilder::finalize_sync_nodes(DagGraph & graph, const BuildIndex & index) const {
    /**
     * @brief Replaces synchronization waits with fixed execution overhead.
     *
     * The backend assigns synchronization waits a fixed 10-microsecond execution cost. Their
     * observed blocking duration is represented by predecessor edges instead of the node,
     * allowing the critical path to express waiting without double-counting observed duration.
     */
    auto set_fixed_sync_duration = [&](const std::vector<size_t> & nodes) {
        std::ranges::for_each(nodes, [&](size_t node_id) { graph.set_node_duration(node_id, 10); });
    };
    set_fixed_sync_duration(index.stream_sync_nodes);
    set_fixed_sync_duration(index.event_sync_nodes);
    set_fixed_sync_duration(index.device_sync_nodes);
    set_fixed_sync_duration(index.event_wait_nodes);
    set_fixed_sync_duration(index.notify_wait_nodes);
}

} // namespace markov::trace_graph::core
