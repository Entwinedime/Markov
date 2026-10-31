#include "modules/hicache/phase_observation.hpp"

#include "markov/trace_graph/adapters/frameworks/sglang/attention_program.hpp"
#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/layer_waits.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace markov::trace_graph::modules::hicache {
namespace {
using core::TraceEvent;

bool is_device_control_anchor(std::string_view name) {
    return name == "EVENT_WAIT" || name == "EVENT_RECORD" || name == "NOTIFY_WAIT" || name == "NOTIFY_RECORD" || name == "MODEL_EXECUTE";
}

bool is_phase_cpu_control(const TraceEvent & event) {
    return event.name.starts_with("Enqueue@") || event.name == "Node@launch" || event.cat == "enqueue" || event.name.starts_with("AscendCL@aclrtLaunch")
           || event.name.starts_with("AscendCL@aclrtMemcpyAsync") || event.name == "AscendCL@aclrtRecordEvent" || event.name == "AscendCL@aclrtWaitEvent";
}

bool is_collective_node(std::string_view name) { return name.starts_with("hcom"); }

std::string phase_family_name(std::string_view name) {
    const auto instance_suffix = name.find("__");
    return std::string(name.substr(0, instance_suffix));
}

} // namespace

HiCachePhaseObservationAudit observe_hicache_phases(const core::DagGraph & graph, const HiCacheLayerWaitObservation * layer_waits) {
    auto input = observe_hicache_phase_inputs(graph.source_observations());
    auto & audit = input.audit;
    const auto & phase_intervals = input.intervals;
    HiCacheLayerWaitObservation local_waits;
    if (!layer_waits) {
        if (std::ranges::any_of(graph.source_observations().runtime_observations,
                                [](const auto & event) { return event.name == "runtime.hicache.layer_waits"; }))
            local_waits = observe_hicache_layer_waits(source_dag::SourceDagIndex(graph));
        layer_waits = &local_waits;
    }
    audit.layer_wait_status = layer_waits->status;
    audit.layer_wait_issues = layer_waits->issues;
    using FineTime = unsigned __int128;
    const auto fine_start = [](const TraceEvent & event) { return FineTime(event.ts) * 1'000 + event.ts_submicro_ns; };
    const auto fine_end = [&](const TraceEvent & event) { return fine_start(event) + FineTime(event.dur) * 1'000 + event.dur_submicro_ns; };
    struct PhaseOwnerIndex {
        std::vector<const HiCachePhaseInterval *> intervals;
        std::vector<uint64_t> prefix_end_us;
    };
    std::map<int, PhaseOwnerIndex> phase_owners;
    for (const auto & interval : phase_intervals) phase_owners[interval.logical_input].intervals.push_back(&interval);
    for (auto & [input, index] : phase_owners) {
        (void)input;
        std::ranges::sort(index.intervals, {}, &HiCachePhaseInterval::start_us);
        uint64_t end_us = 0;
        for (const auto * interval : index.intervals) {
            end_us = std::max(end_us, interval->end_us);
            index.prefix_end_us.push_back(end_us);
        }
    }

    const auto find_owner = [&](const core::DagNode & node, uint64_t anchor_us) {
        const auto found = phase_owners.find(node.gpu_id);
        if (found == phase_owners.end()) return static_cast<const HiCachePhaseInterval *>(nullptr);

        const auto & index = found->second;
        auto at = std::ranges::upper_bound(index.intervals, anchor_us, {}, &HiCachePhaseInterval::start_us);
        const HiCachePhaseInterval * owner = nullptr;
        // Earlier intervals whose cumulative end has passed cannot contain
        // this anchor. Keep checking all overlapping candidates: overlap is
        // still an ownership conflict, not permission to choose the latest.
        while (at != index.intervals.begin()) {
            const auto position = static_cast<size_t>(std::distance(index.intervals.begin(), --at));
            if (index.prefix_end_us[position] <= anchor_us) break;
            if (anchor_us >= (*at)->end_us) continue;

            if (owner != nullptr) {
                ++audit.phase_owner_conflict_count;
                return static_cast<const HiCachePhaseInterval *>(nullptr);
            }
            owner = *at;
        }
        return owner;
    };

    std::unordered_map<size_t, std::vector<size_t>> correlation_predecessors;
    for (const auto & edge : graph.edges()) {
        if (edge.active && edge.kind == core::DagEdgeKind::Correlation) correlation_predecessors[edge.dst].push_back(edge.src);
    }
    // CANN gives Node@launch a synthetic activity PID, whereas torch Dequeue
    // uses the real process PID. They still identify the same OS worker thread.
    // Retain containment only as an ownership link: adding a hard DAG edge from
    // the enclosing Dequeue's END to its nested launch would be incorrect.
    std::map<std::pair<int, std::string>, std::vector<size_t>> worker_dequeues;
    for (const auto & node : graph.nodes()) {
        if (!node.active || !node.is_cpu || node.kind != core::DagNodeKind::TraceEvent) continue;
        const auto & event = graph.event_for_node(node.id);
        if (event.source_channel == core::TraceSourceChannel::Torch && event.name.starts_with("Dequeue@") && event.has_arg("correlation_id"))
            worker_dequeues[{ node.gpu_id, event.tid }].push_back(node.id);
    }
    for (auto & [key, lane] : worker_dequeues) {
        std::ranges::sort(lane, [&](size_t a, size_t b) { return fine_start(graph.event_for_node(a)) < fine_start(graph.event_for_node(b)); });
        // An overlapping queue is ambiguous, not permission to choose the latest.
        for (size_t i = 1; i < lane.size(); ++i) {
            if (fine_end(graph.event_for_node(lane[i - 1])) > fine_start(graph.event_for_node(lane[i]))) {
                lane.clear();
                break;
            }
        }
    }
    std::unordered_map<size_t, size_t> launch_dequeue;
    for (const auto & node : graph.nodes()) {
        if (!node.active || !node.is_cpu || node.kind != core::DagNodeKind::TraceEvent) continue;
        const auto & event = graph.event_for_node(node.id);
        if (event.source_channel != core::TraceSourceChannel::Torch || event.name != "Node@launch" || event.arg("Thread Id") != event.tid) continue;
        const auto found = worker_dequeues.find({ node.gpu_id, event.tid });
        if (found == worker_dequeues.end()) continue;
        const auto & lane = found->second;
        auto at = std::ranges::upper_bound(lane, fine_start(event), {}, [&](size_t id) { return fine_start(graph.event_for_node(id)); });
        if (at == lane.begin()) continue;
        const auto parent = *--at;
        if (fine_end(event) <= fine_end(graph.event_for_node(parent))) launch_dequeue.emplace(node.id, parent);
    }
    // Both device execution and the worker's launch may outlive the producer's
    // marker. Prefer the original Enqueue on the proven correlation chain, but
    // leave physical submit_ts and DAG dependencies untouched. An observed
    // producer outside all phases must not fall back to a coincident launch.
    const auto device_owner = [&](const core::DagNode & node) -> const HiCachePhaseInterval * {
        std::optional<const HiCachePhaseInterval *> producer;
        std::vector<size_t> pending{ node.id };
        std::unordered_set<size_t> visited{ node.id };
        while (!pending.empty()) {
            const auto id = pending.back();
            pending.pop_back();
            if (const auto parent = launch_dequeue.find(id); parent != launch_dequeue.end() && visited.insert(parent->second).second)
                pending.push_back(parent->second);
            const auto found = correlation_predecessors.find(id);
            if (found == correlation_predecessors.end()) continue;
            for (const auto predecessor_id : found->second) {
                if (!visited.insert(predecessor_id).second) continue;
                const auto & predecessor = graph.node(predecessor_id);
                if (!predecessor.active || predecessor.kind != core::DagNodeKind::TraceEvent || predecessor.gpu_id != node.gpu_id) continue;
                const auto & event = graph.event_for_node(predecessor_id);
                if (predecessor.is_cpu && event.source_channel == core::TraceSourceChannel::Torch
                    && (event.name.starts_with("Enqueue@") || event.cat == "enqueue")) {
                    const auto * owner = find_owner(predecessor, event.ts);
                    if (owner != nullptr && (event.pid != owner->pid || event.tid != owner->tid)) owner = nullptr;
                    if (producer && *producer != owner) {
                        ++audit.phase_owner_conflict_count;
                        return nullptr;
                    }
                    producer = owner;
                }
                pending.push_back(predecessor_id);
            }
        }
        return producer ? *producer : find_owner(node, node.submit_ts);
    };
    std::map<size_t, const HiCachePhaseInterval *> device_owners;
    for (const auto & node : graph.nodes()) {
        if (!node.active || node.is_cpu || node.kind != core::DagNodeKind::TraceEvent || node.submit_ts == 0) continue;
        const auto * owner = device_owner(node);
        if (owner == nullptr) continue;
        device_owners.emplace(node.id, owner);
        const auto & event = graph.event_for_node(node.id);
        auto & observation = audit.observations[owner->observation_index];
        if (owner->kind == HiCachePhaseKind::Decode) observation.decode_node_iterations.emplace(node.id, owner->decode_iteration);
        ++audit.phase_owned_device_node_count;
        auto & family = owner->kind == HiCachePhaseKind::Prefill ? audit.prefill_device_families[phase_family_name(event.name)]
                                                                 : audit.decode_device_families[phase_family_name(event.name)];
        ++family.node_count;
        family.duration_us = core::checked_add_u64(family.duration_us, node.duration, "HiCache phase family duration exceeds uint64 range");
        auto & phase = owner->kind == HiCachePhaseKind::Prefill ? observation.prefill : observation.decode;
        ++phase.device_node_count;
        phase.device_duration_us = core::checked_add_u64(phase.device_duration_us, node.duration, "HiCache phase device duration exceeds uint64 range");
        if (is_device_control_anchor(event.name)) continue;

        ++phase.compute_node_count;
        phase.compute_duration_us = core::checked_add_u64(phase.compute_duration_us, node.duration, "HiCache phase compute duration exceeds uint64 range");
        if (is_collective_node(event.name)) {
            phase.collective_node_ids.push_back(node.id);
            phase.collective_duration_us =
                core::checked_add_u64(phase.collective_duration_us, node.duration, "HiCache phase collective duration exceeds uint64 range");
            continue;
        }

        auto & kernel_family = phase.kernel_families[phase_family_name(event.name)];
        ++kernel_family.node_count;
        kernel_family.duration_us = core::checked_add_u64(kernel_family.duration_us, node.duration, "HiCache kernel family duration exceeds uint64 range");
        phase.kernel_duration_us = core::checked_add_u64(phase.kernel_duration_us, node.duration, "HiCache phase kernel duration exceeds uint64 range");
        if (owner->kind == HiCachePhaseKind::Decode) { observation.decode_kernel_node_ids.push_back(node.id); }
        else if (adapters::sglang::is_attention_kernel(event.name)) {
            observation.prefill_prefix_attention_node_ids.push_back(node.id);
            observation.prefill_prefix_attention_duration_us = core::checked_add_u64(observation.prefill_prefix_attention_duration_us,
                                                                                     node.duration,
                                                                                     "HiCache prefill prefix-attention duration exceeds uint64 range");
        }
        else {
            observation.prefill_common_kernel_node_ids.push_back(node.id);
            observation.prefill_common_kernel_duration_us = core::checked_add_u64(observation.prefill_common_kernel_duration_us,
                                                                                  node.duration,
                                                                                  "HiCache prefill common-kernel duration exceeds uint64 range");
        }
    }

    // Direct submissions belong to the marker's caller thread. Background
    // cleanup in the same process is not phase work merely because it overlaps
    // EXTEND/DECODE. Worker ownership is added below through correlation chains;
    // unowned work remains in the graph, with its cost and dependencies intact.
    std::map<size_t, const HiCachePhaseInterval *> cpu_owners;
    for (const auto & node : graph.nodes()) {
        if (!node.active || !node.is_cpu || node.kind != core::DagNodeKind::TraceEvent) continue;
        if (layer_waits->cpu_node_ids.contains(node.id)) continue;
        const auto & event = graph.event_for_node(node.id);
        if (event.source_channel != core::TraceSourceChannel::Torch || !is_phase_cpu_control(event)) continue;
        const auto * owner = find_owner(node, event.ts);
        if (owner == nullptr) continue;
        const auto end_us = core::checked_add_u64(event.ts, event.dur, "HiCache phase CPU control end exceeds uint64 range");
        if (event.pid == owner->pid && event.tid == owner->tid && end_us <= owner->end_us) cpu_owners.emplace(node.id, owner);
    }
    std::unordered_set<size_t> proven_cpu_owners;
    for (const auto & [device_id, owner] : device_owners) {
        std::vector<size_t> pending{ device_id };
        std::unordered_set<size_t> visited{ device_id };
        while (!pending.empty()) {
            const auto node_id = pending.back();
            pending.pop_back();
            const auto found = correlation_predecessors.find(node_id);
            if (found == correlation_predecessors.end()) continue;
            for (const auto predecessor_id : found->second) {
                if (!visited.insert(predecessor_id).second) continue;
                const auto & predecessor = graph.node(predecessor_id);
                if (!predecessor.active || predecessor.kind != core::DagNodeKind::TraceEvent) continue;
                if (!predecessor.is_cpu) {
                    pending.push_back(predecessor_id);
                    continue;
                }
                const auto & event = graph.event_for_node(predecessor_id);
                if (event.source_channel != core::TraceSourceChannel::Torch || predecessor.gpu_id != owner->logical_input || event.pid != owner->pid
                    || layer_waits->cpu_node_ids.contains(predecessor_id))
                    continue;
                // A delayed worker anchor follows its producer, not whichever
                // marker happens to overlap its launch. Conflicting device
                // consumers remain an error rather than picking one owner.
                if (proven_cpu_owners.insert(predecessor_id).second) cpu_owners[predecessor_id] = owner;
                else if (cpu_owners.at(predecessor_id) != owner) ++audit.phase_owner_conflict_count;
            }
        }
    }
    for (const auto & [node_id, owner] : cpu_owners) {
        const auto & node = graph.node(node_id);
        auto & observation = audit.observations[owner->observation_index];
        if (owner->kind == HiCachePhaseKind::Decode) observation.decode_node_iterations.emplace(node_id, owner->decode_iteration);
        ++audit.phase_owned_submit_cpu_node_count;
        auto & phase = owner->kind == HiCachePhaseKind::Prefill ? observation.prefill : observation.decode;
        phase.submit_cpu_node_ids.push_back(node_id);
        phase.submit_cpu_duration_us = core::checked_add_u64(phase.submit_cpu_duration_us, node.duration, "HiCache phase CPU duration exceeds uint64 range");
    }

    if (audit.phase_owner_conflict_count) audit.status = "not_ready";
    return std::move(audit);
}

} // namespace markov::trace_graph::modules::hicache
