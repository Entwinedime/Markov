#include "markov/trace_graph/modules/hicache/patch/layer_wait_insertion.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include <algorithm>
#include <array>
#include <set>
#include <tuple>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::patch {
namespace {
using Ref = core::DagNodeRef;
using Kind = core::DagEdgeKind;
using SampleKey = std::pair<int, std::string>;
constexpr const char * effect = "hicache_layer_wait";
uint64_t start_ns(const core::TraceEvent & e) { return e.ts * 1'000 + e.ts_submicro_ns; }
uint64_t end_ns(const core::TraceEvent & e) { return start_ns(e) + e.dur * 1'000 + e.dur_submicro_ns; }
struct CallTemplate {
    size_t main_lane = 0, worker_lane = 0, device_lane = 0;
    // Before enqueue, enqueue, after enqueue, worker service, worker ready delay.
    std::array<std::vector<uint64_t>, 5> samples;
    std::array<uint64_t, 5> cost{}, remainder{};
};

// CPU program order is the insertion clock, even when the device is far behind.
std::optional<size_t> main_submission(const HiCacheSourceDagIndex & source, size_t id, size_t lane) {
    const auto & graph = source.graph();
    if (graph.node(id).is_cpu && graph.node(id).lane_id == lane) return id;
    std::set<size_t> found;
    std::vector<size_t> frontier{ id };
    std::set<size_t> visited;
    while (!frontier.empty()) {
        const auto current = frontier.back();
        frontier.pop_back();
        if (!visited.insert(current).second) continue;
        for (const auto edge_id : source.incoming_edge_ids(current)) {
            const auto & edge = graph.edge(edge_id);
            if (!edge.active || edge.kind != Kind::Correlation) continue;
            if (graph.node(edge.src).is_cpu && graph.node(edge.src).lane_id == lane) found.insert(edge.src);
            else frontier.push_back(edge.src);
        }
    }
    return found.size() == 1 ? std::optional{ *found.begin() } : std::nullopt;
}

struct DevicePlacement {
    std::vector<size_t> before, syncs;
    size_t after = 0;
};
struct DeviceSubmission {
    uint64_t begin = 0;
    size_t device = 0;
};

std::optional<DevicePlacement> placement(const HiCacheSourceDagIndex & source, const HiCacheLayerWaitCall & call, const CallTemplate & sample,
                                         const std::vector<DeviceSubmission> & submissions, std::string & issue) {
    const auto & graph = source.graph();
    const auto next = std::ranges::lower_bound(submissions, call.end_ns, {}, &DeviceSubmission::begin);
    if (next == submissions.end()) {
        issue = "device_successor_missing";
        return std::nullopt;
    }
    DevicePlacement result{ .after = next->device };
    for (const auto edge_id : source.incoming_edge_ids(next->device)) {
        const auto & edge = graph.edge(edge_id);
        if (!edge.active || edge.kind != Kind::Stream) continue;
        if (graph.node(edge.src).lane_id != sample.device_lane) {
            issue = "device_predecessor_other_lane";
            return std::nullopt;
        }
        const auto submit = main_submission(source, edge.src, sample.main_lane);
        if (!submit) {
            issue = "device_predecessor_submission_missing";
            return std::nullopt;
        }
        if (end_ns(graph.event_for_node(*submit)) > call.start_ns) {
            issue = "device_predecessor_crosses_call";
            return std::nullopt;
        }
        result.before.push_back(edge.src);
        for (const auto outgoing : source.outgoing_edge_ids(edge.src)) {
            const auto & sync = graph.edge(outgoing);
            if (!sync.active || sync.kind != Kind::Sync) continue;
            // A wait for an already recorded event does not cover later stream work.
            if (!core::synchronizes_device_frontier(graph.event_for_node(sync.dst))) continue;
            const auto consumer = main_submission(source, sync.dst, sample.main_lane);
            if (!consumer) {
                issue = "sync_consumer_submission_missing";
                return std::nullopt;
            }
            const auto & event = graph.event_for_node(*consumer);
            if (start_ns(event) >= call.end_ns) result.syncs.push_back(sync.dst);
            else if (end_ns(event) > call.start_ns) {
                issue = "sync_submission_crosses_call";
                return std::nullopt;
            }
        }
    }
    std::ranges::sort(result.before);
    result.before.erase(std::unique(result.before.begin(), result.before.end()), result.before.end());
    std::ranges::sort(result.syncs);
    result.syncs.erase(std::unique(result.syncs.begin(), result.syncs.end()), result.syncs.end());
    return result;
}
} // namespace

std::map<const HiCacheLayerWaitCall *, HiCacheLayerWaitPosition> observe_inactive_layer_positions(
    const HiCacheSourceDagIndex & source, const HiCacheLayerWaitObservation & waits,
    std::span<const HiCacheLayerWaitCall * const> calls) {
    const auto & graph = source.graph();
    std::map<SampleKey, CallTemplate> samples;
    for (const auto & call : waits.calls) {
        if (!call.enabled || !call.issue.empty()) continue;
        samples.try_emplace({call.logical_input, call.phase}, CallTemplate{
            .main_lane = graph.node(*call.submission).lane_id,
            .worker_lane = graph.node(*call.worker).lane_id,
            .device_lane = graph.node(*call.device_wait).lane_id});
    }
    using Lanes = std::pair<size_t, size_t>;
    std::map<Lanes, std::vector<DeviceSubmission>> indices;
    std::map<size_t, std::vector<DeviceSubmission>> ordinary_submissions;
    for (const auto * call : calls) {
        if (!samples.contains({call->logical_input, call->phase})) {
            ordinary_submissions.try_emplace(graph.node(*call->before).lane_id);
            continue;
        }
        const auto & sample = samples.at({call->logical_input, call->phase});
        indices.try_emplace({sample.main_lane, sample.device_lane});
    }
    // A disabled wait has no donor, but the next ordinary device submission
    // in this forward still identifies the stream consuming that layer's KV.
    // Use CPU submission order, never GPU execution timestamps or target trace.
    for (auto & [main_lane, entries] : ordinary_submissions) {
        for (const auto & node : graph.nodes()) {
            if (!node.active || node.is_cpu) continue;
            const auto & name = graph.event_for_node(node.id).name;
            if (name == "EVENT_RECORD" || name == "EVENT_WAIT" || name == "logical_event_wait") continue;
            if (const auto submit = main_submission(source, node.id, main_lane)) {
                entries.push_back({start_ns(graph.event_for_node(*submit)), node.id});
                indices.try_emplace({main_lane, node.lane_id});
            }
        }
        std::ranges::sort(entries, {}, [](const auto & entry) { return std::pair{entry.begin, entry.device}; });
    }
    for (auto & [lanes, entries] : indices) {
        for (const auto & node : graph.nodes()) {
            if (!node.active || node.is_cpu || node.lane_id != lanes.second) continue;
            if (const auto submit = main_submission(source, node.id, lanes.first))
                entries.push_back({start_ns(graph.event_for_node(*submit)), node.id});
        }
        std::ranges::sort(entries, {}, [](const auto & entry) { return std::pair{entry.begin, entry.device}; });
    }
    std::map<const HiCacheLayerWaitCall *, HiCacheLayerWaitPosition> result;
    for (const auto * call : calls) {
        CallTemplate sample;
        if (!samples.contains({call->logical_input, call->phase})) {
            const auto main_lane = graph.node(*call->before).lane_id;
            const auto & entries = ordinary_submissions.at(main_lane);
            const auto next = std::ranges::lower_bound(entries, call->end_ns, {}, &DeviceSubmission::begin);
            uint64_t next_layer = UINT64_MAX;
            for (const auto & other : waits.calls)
                if (other.logical_input == call->logical_input && other.batch_start_ns == call->batch_start_ns
                    && other.layer > call->layer && other.start_ns > call->start_ns)
                    next_layer = std::min(next_layer, other.start_ns);
            if (next == entries.end() || next->begin >= next_layer) {
                result.emplace(call, HiCacheLayerWaitPosition{.issue = "forward_device_submission_missing"});
                continue;
            }
            sample.main_lane = main_lane;
            sample.device_lane = graph.node(next->device).lane_id;
            bool ambiguous = false;
            for (auto candidate = next; candidate != entries.end() && candidate->begin == next->begin; ++candidate)
                ambiguous |= graph.node(candidate->device).lane_id != sample.device_lane;
            if (ambiguous) {
                result.emplace(call, HiCacheLayerWaitPosition{.issue = "forward_submission_has_multiple_streams"});
                continue;
            }
        } else sample = samples.at({call->logical_input, call->phase});
        std::string issue;
        const auto position = placement(source, *call, sample, indices.at({sample.main_lane, sample.device_lane}), issue);
        if (!position) result.emplace(call, HiCacheLayerWaitPosition{ .issue = std::move(issue) });
        else result.emplace(call, HiCacheLayerWaitPosition{position->before, position->syncs, position->after, sample.device_lane, {}});
    }
    return result;
}

HiCacheLayerWaitInsertionPlan plan_hicache_layer_wait_insertions(const HiCacheSourceDagIndex & source, const HiCacheLayerWaitObservation & waits,
                                                                 const std::vector<HiCacheLayerWaitInsertion> & insertions) {
    HiCacheLayerWaitInsertionPlan result;
    if (insertions.empty()) {
        result.status = "ready";
        return result;
    }
    if (waits.status != "ready") {
        result.issues["source_calls_incomplete"] = 1;
        result.status = "blocked";
        return result;
    }
    const auto & graph = source.graph();
    std::map<SampleKey, CallTemplate> templates;
    for (const auto & call : waits.calls) {
        if (!call.enabled || !call.issue.empty()) continue;
        const auto & submit = graph.event_for_node(*call.submission);
        const auto & worker = graph.event_for_node(*call.worker);
        const CallTemplate lanes{ .main_lane = graph.node(*call.submission).lane_id,
                                  .worker_lane = graph.node(*call.worker).lane_id,
                                  .device_lane = graph.node(*call.device_wait).lane_id };
        auto [found, first] = templates.try_emplace({ call.logical_input, call.phase }, lanes);
        auto & sample = found->second;
        if (std::tie(sample.main_lane, sample.worker_lane, sample.device_lane) != std::tie(lanes.main_lane, lanes.worker_lane, lanes.device_lane)) {
            ++result.issues["active_call_lane_ambiguous"];
            continue;
        }
        const auto queue = source.cpu_nodes_on_lane(worker.pid, worker.tid);
        const auto at =
            std::ranges::lower_bound(queue, std::pair{ worker.ts, *call.worker }, {}, [&](size_t id) { return std::pair{ graph.event_for_node(id).ts, id }; });
        if (at == queue.end() || *at != *call.worker) {
            ++result.issues["worker_source_order_missing"];
            continue;
        }
        const auto previous_end = at == queue.begin() ? 0 : end_ns(graph.event_for_node(*std::prev(at)));
        const auto ready = std::max(previous_end, end_ns(submit));
        const std::array<uint64_t, 5> values{ start_ns(submit) - call.start_ns,
                                              end_ns(submit) - start_ns(submit),
                                              call.end_ns - end_ns(submit),
                                              end_ns(worker) - start_ns(worker),
                                              start_ns(worker) > ready ? start_ns(worker) - ready : 0 };
        for (size_t i = 0; i < values.size(); ++i) sample.samples[i].push_back(values[i]);
    }
    for (auto & [key, sample] : templates)
        for (size_t i = 0; i < sample.cost.size(); ++i) {
            auto & values = sample.samples[i];
            if (values.empty()) {
                ++result.issues["active_call_sample_missing"];
                continue;
            }
            std::ranges::sort(values);
            const auto middle = values.size() / 2;
            sample.cost[i] = values.size() % 2 ? values[middle] : values[middle - 1] + (values[middle] - values[middle - 1]) / 2;
        }
    using Lanes = std::pair<size_t, size_t>;
    std::map<Lanes, std::vector<DeviceSubmission>> device_submissions;
    for (const auto & [key, sample] : templates) device_submissions.try_emplace({ sample.main_lane, sample.device_lane });
    for (auto & [lanes, submissions] : device_submissions) {
        for (const auto & node : graph.nodes()) {
            if (!node.active || node.is_cpu || node.lane_id != lanes.second) continue;
            const auto submit = main_submission(source, node.id, lanes.first);
            if (submit) submissions.push_back({ start_ns(graph.event_for_node(*submit)), node.id });
        }
        std::ranges::sort(submissions, {}, [](const auto & item) { return std::pair{ item.begin, item.device }; });
    }
    std::map<std::pair<size_t, size_t>, std::vector<const HiCacheLayerWaitInsertion *>> gaps;
    for (const auto & item : insertions) {
        if (!item.call || item.call->enabled || !item.call->issue.empty() || !item.call->before || !item.call->after) {
            ++result.issues["insertion_call_not_inactive"];
            continue;
        }
        gaps[{ *item.call->before, *item.call->after }].push_back(&item);
    }
    auto & plan = result.mutation;
    const auto connect = [&](Ref from, Ref to, Kind kind) {
        plan.add_edges.push_back({ .src = std::move(from), .dst = std::move(to), .kind = kind, .effect_id = effect });
    };
    std::map<Lanes, Ref> preceding_wait;
    std::map<size_t, uint64_t> outside_remainders;
    std::vector<decltype(gaps)::iterator> groups;
    for (auto at = gaps.begin(); at != gaps.end(); ++at) {
        std::ranges::sort(at->second, {}, [](const auto * item) { return item->call->start_ns; });
        groups.push_back(at);
    }
    std::ranges::sort(groups, {}, [](const auto & group) { return group->second.front()->call->start_ns; });
    for (const auto group : groups) {
        auto & [boundary, calls] = *group;
        const auto [before, after] = boundary;
        const auto gap_start = graph.event_for_node(before).ts + graph.event_for_node(before).dur;
        const auto gap_end = graph.event_for_node(after).ts;
        std::vector<size_t> sequential;
        for (const auto id : source.outgoing_edge_ids(before))
            if (graph.edge(id).active && graph.edge(id).kind == Kind::Sequential) sequential.push_back(id);
        if (sequential.size() != 1 || graph.edge(sequential.front()).dst != after || gap_end < gap_start
            || graph.node(before).cpu_gap_after != gap_end - gap_start || graph.node(before).lane_id != graph.node(after).lane_id) {
            ++result.issues["inactive_call_gap_not_unique"];
            continue;
        }
        plan.disable_edges.push_back(sequential.front());
        // Quantize the retained intervals cumulatively across this main lane.
        // Per-call rounding would retain or remove every sub-microsecond call.
        auto & outside_remainder = outside_remainders[graph.node(before).lane_id];
        const auto outside_us = [&](uint64_t ns) {
            const auto total = outside_remainder + ns;
            outside_remainder = total % 1'000;
            return total / 1'000;
        };
        plan.set_cpu_gaps.push_back({ .node_id = before, .duration = outside_us(calls.front()->call->start_ns - gap_start * 1'000), .effect_id = effect });
        Ref previous = Ref::existing(before);
        uint64_t previous_end_ns = gap_start * 1'000;
        for (size_t i = 0; i < calls.size(); ++i) {
            const auto & call = *calls[i]->call;
            auto found = templates.find({ call.logical_input, call.phase });
            if (found == templates.end()) {
                ++result.issues["active_call_sample_missing"];
                continue;
            }
            auto & sample = found->second;
            const Lanes lanes{ sample.main_lane, sample.device_lane };
            if (call.start_ns < previous_end_ns || call.end_ns > gap_end * 1'000 || graph.node(before).lane_id != sample.main_lane) {
                ++result.issues["inactive_call_outside_gap"];
                continue;
            }
            std::string issue;
            const auto where = placement(source, call, sample, device_submissions.at(lanes), issue);
            if (!where) {
                ++result.issues[issue];
                continue;
            }
            const auto next_begin = i + 1 < calls.size() ? calls[i + 1]->call->start_ns : gap_end * 1'000;
            const auto prefix = std::string(effect) + ":" + std::to_string(call.logical_input) + ":" + std::to_string(call.start_ns);
            std::array<uint64_t, 5> costs;
            for (size_t j = 0; j < costs.size(); ++j) {
                const auto total = sample.cost[j] + sample.remainder[j];
                costs[j] = total / 1'000;
                sample.remainder[j] = total % 1'000;
            }
            const auto node = [&](const char * part, size_t lane, bool cpu, uint64_t cost, uint64_t gap = 0, std::optional<uint64_t> ready = std::nullopt) {
                const auto name = prefix + ":" + part;
                plan.synthetic_nodes.push_back({
                    .synthetic_id = name,
                    .node = { .name = std::string(effect) + "_" + part,
                             .is_cpu = cpu,
                             .lane_key = std::string(graph.lane_key(lane)),
                             .duration = cost,
                             .attrs = { { "request_id", call.request_id }, { "phase", call.phase }, { "layer", std::to_string(call.layer) } },
                             .cpu_task_ready_delay_us = ready,
                             .cpu_gap_after = gap },
                    .effect_id = effect
                });
                return Ref::synthetic(name);
            };
            const auto pre = node("before_submit", sample.main_lane, true, costs[0]);
            const auto submit = node("submit", sample.main_lane, true, costs[1]);
            const auto post = node("after_submit", sample.main_lane, true, costs[2], outside_us(next_begin - call.end_ns));
            const auto worker = node("worker", sample.worker_lane, true, costs[3], 0, costs[4]);
            const auto wait = node("wait", sample.device_lane, false, 0);
            result.waits.emplace(&call, wait);
            connect(previous, pre, Kind::Sequential);
            connect(pre, submit, Kind::Sequential);
            connect(submit, post, Kind::Sequential);
            connect(submit, worker, Kind::Correlation);
            connect(worker, wait, Kind::Correlation);
            connect(calls[i]->layer_ready, wait, Kind::Sync);
            for (const auto id : where->before) connect(Ref::existing(id), wait, Kind::Stream);
            if (const auto prior = preceding_wait.find(lanes); prior != preceding_wait.end()) connect(prior->second, wait, Kind::Stream);
            connect(wait, Ref::existing(where->after), Kind::Stream);
            for (const auto id : where->syncs) connect(wait, Ref::existing(id), Kind::Sync);
            preceding_wait[lanes] = wait;
            previous = post;
            previous_end_ns = call.end_ns;
            ++result.inserted_calls;
        }
        connect(previous, Ref::existing(after), Kind::Sequential);
    }
    result.status = result.issues.empty() ? "ready" : "blocked";
    if (!result.issues.empty()) {
        result.mutation = { .component = effect };
        result.inserted_calls = 0;
        result.waits.clear();
    }
    return result;
}
} // namespace markov::trace_graph::modules::hicache::patch
