/**
 * @file
 * @brief Builds runtime-to-device causality chains and submission metadata.
 */
#include "dag_builder_stages.hpp"

#include <algorithm>
#include <future>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace markov::trace_graph::core {

namespace {

bool is_submit_anchor_event(const TraceEvent & event) {
    if (event.name.starts_with("Enqueue@")) return true;
    if (event.name == "Node@launch") return true;
    if (event.cat == "enqueue") return true;
    if (event.name.starts_with("AscendCL@aclrtLaunch")) return true;
    if (event.name.starts_with("AscendCL@aclrtMemcpyAsync")) return true;
    if (event.name == "AscendCL@aclrtRecordEvent" || event.name == "AscendCL@aclrtWaitEvent") return true;
    return false;
}

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

using NodeGroups = std::unordered_map<std::string, std::vector<size_t>>;
using CausalityEntry = std::pair<std::string, std::vector<size_t> *>;

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

} // namespace

void add_correlation_edges(DagGraph & graph, DagBuildIndex & index, size_t threads) {
    /**
     * @brief Builds CPU-runtime to device-kernel submission chains by correlation ID.
     *
     * Torch profiler commonly shares this identity across launch, runtime, and kernel
     * events. Timestamp order within one identity defines the submission chain.
     */
    auto correlation_entries = multi_node_entries(index.correlation_to_nodes);
    auto correlation_results =
        map_entries_parallel<CorrelationGroupResult>(correlation_entries, threads, [&](const auto & item) { return build_correlation_group(graph, item); });

    /**
     * @brief Builds CANN runtime-to-device chains by connection ID.
     *
     * Chains of three or more events are rejected when they do not begin with
     * `Node@launch`; such groups frequently combine unrelated runtime events.
     */
    auto connection_entries = multi_node_entries(index.connection_to_nodes);
    auto connection_results =
        map_entries_parallel<CorrelationGroupResult>(connection_entries, threads, [&](const auto & item) { return build_connection_group(graph, item); });
    apply_causality_results(graph, correlation_results);
    apply_causality_results(graph, connection_results);
}

} // namespace markov::trace_graph::core
