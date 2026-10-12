#include "markov/trace_graph/modules/hicache/patch/layer_wait_patch.hpp"
#include "markov/trace_graph/modules/hicache/patch/layer_io.hpp"
#include "markov/trace_graph/modules/hicache/patch/layer_wait_insertion.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>
#include <tuple>

namespace markov::trace_graph::modules::hicache::patch {
namespace {
using Ref = core::DagNodeRef;
using Kind = core::DagEdgeKind;
using Request = std::pair<std::string, std::string>;
using Sample = std::pair<int, std::string>;
constexpr const char * effect = "hicache_layer_wait";

struct RequestLoadbacks {
    std::vector<HiCacheRewriteDecision *> opportunities;
    size_t next = 0;
    std::optional<uint64_t> batch;
    std::optional<bool> enabled;
    HiCacheRewriteDecision * active = nullptr;
};

// Keep stream order and downstream synchronizations, but not the removed
// wait's Record dependency. Adjacent removed waits are traversed together.
void bypass_device_waits(const HiCacheSourceDagIndex & source, const std::set<size_t> & removed, core::DagMutationPlan & plan) {
    const auto & graph = source.graph();
    std::set<std::tuple<size_t, size_t, Kind>> additions;
    for (const auto id : removed) {
        for (const auto outgoing : source.outgoing_edge_ids(id)) {
            const auto & after = graph.edge(outgoing);
            if (!after.active || removed.contains(after.dst)) continue;
            if (after.kind != Kind::Stream && after.kind != Kind::Sync) throw std::logic_error("layer wait has an unsupported outgoing dependency");
            std::vector<size_t> frontier{ id };
            std::set<size_t> visited;
            while (!frontier.empty()) {
                const auto current = frontier.back();
                frontier.pop_back();
                if (!visited.insert(current).second) continue;
                for (const auto incoming : source.incoming_edge_ids(current)) {
                    const auto & before = graph.edge(incoming);
                    if (!before.active || before.kind != Kind::Stream) continue;
                    if (removed.contains(before.src)) frontier.push_back(before.src);
                    else additions.emplace(before.src, after.dst, after.kind);
                }
            }
        }
    }
    for (const auto & [before, after, kind] : additions) {
        const auto exists = std::ranges::any_of(source.outgoing_edge_ids(before), [&](size_t id) {
            const auto & edge = graph.edge(id);
            return edge.active && edge.dst == after && edge.kind == kind && edge.effect_id().empty();
        });
        if (!exists)
            plan.add_edges.push_back({ .src = Ref::existing(before),
                                       .dst = Ref::existing(after),
                                       .kind = kind,
                                       .effect_id = effect,
                                       .reason = "preserve stream work without the obsolete layer wait" });
    }
}
} // namespace

static HiCacheLayerWaitPatch build_layer_wait_plan(const HiCacheSourceDagIndex & source, const HiCacheLayerWaitObservation & waits,
                                                   std::vector<HiCacheRewriteDecision> & decisions, core::DagMutationPlan & plan) {
    HiCacheLayerWaitPatch audit;
    if (waits.status == "unavailable") return audit;
    if (waits.status != "ready") {
        audit.status = "blocked";
        audit.blockers = waits.issues;
        return audit;
    }
    const auto & graph = source.graph();
    std::map<Request, RequestLoadbacks> target;
    for (auto & decision : decisions) {
        if (decision.effect_type != model::HiCacheEffectType::Loadback) continue;
        // Resolve rank from the observed fact, not a guessed PID or scope suffix.
        const auto * fact = source.fact_node(decision.source_fact_node_id);
        if (!fact) {
            ++audit.blockers["loadback_fact_missing"];
            continue;
        }
        const auto state = decision.target_effect_state;
        if (state != model::HiCacheTargetEffectState::Required && state != model::HiCacheTargetEffectState::Partial
            && state != model::HiCacheTargetEffectState::NotRequired) {
            ++audit.blockers["loadback_state_unresolved"];
            continue;
        }
        target[{ fact->pid, decision.request_id }].opportunities.push_back(&decision);
    }
    for (auto & [request, timeline] : target) std::ranges::sort(timeline.opportunities, {}, [](const auto * item) { return item->eligibility_timestamp_us; });
    std::map<Sample, std::vector<uint64_t>> samples;
    for (const auto & call : waits.calls)
        if (call.issue.empty() && !call.enabled) samples[{ call.logical_input, call.phase }].push_back(call.end_ns - call.start_ns);
    std::map<Sample, uint64_t> costs, remainder;
    for (auto & [key, values] : samples) {
        std::ranges::sort(values);
        const auto midpoint = values.size() / 2;
        const auto value = values.size() % 2 ? values[midpoint] : values[midpoint - 1] + (values[midpoint] - values[midpoint - 1]) / 2;
        costs[key] = value;
        audit.inactive_call_median_ns[std::to_string(key.first) + ":" + key.second] = value;
    }
    std::vector<const HiCacheLayerWaitCall *> changes;
    std::vector<std::pair<const HiCacheLayerWaitCall *, HiCacheRewriteDecision *>> bindings;
    std::vector<const HiCacheLayerWaitCall *> ordered;
    for (const auto & call : waits.calls) ordered.push_back(&call);
    std::ranges::sort(ordered, {}, [](const auto * call) { return call->start_ns; });
    for (const auto * observed : ordered) {
        const auto & call = *observed;
        if (!call.issue.empty()) {
            ++audit.blockers[call.issue];
            continue;
        }
        const auto found = target.find({ graph.event_for_node(*call.before).pid, call.request_id });
        if (found == target.end()) {
            ++audit.blockers["request_loadback_decision_missing"];
            continue;
        }
        auto & timeline = found->second;
        if (timeline.batch != call.batch_start_ns) {
            timeline.batch = call.batch_start_ns;
            const auto eligible = [&] {
                return timeline.next < timeline.opportunities.size()
                       && timeline.opportunities[timeline.next]->eligibility_timestamp_us <= call.batch_start_ns / 1'000;
            };
            if (call.phase == "EXTEND") {
                // start_loading drains the pending load queue for this batch.
                // Discovery and admission are distinct opportunities, not duplicates.
                if (!eligible()) {
                    ++audit.blockers["extend_loadback_decision_missing"];
                    timeline.enabled.reset();
                }
                else {
                    timeline.enabled = false;
                    timeline.active = nullptr;
                    while (eligible()) {
                        auto * decision = timeline.opportunities[timeline.next++];
                        if (decision->target_effect_state == model::HiCacheTargetEffectState::NotRequired) continue;
                        *timeline.enabled = true;
                        if (timeline.active) ++audit.blockers["multiple_loadbacks_in_one_forward_need_merged_layout"];
                        timeline.active = decision;
                    }
                }
            }
            else if (call.phase != "DECODE" || eligible()) {
                ++audit.blockers["loadback_outside_extend_boundary"];
                timeline.enabled.reset();
            }
        }
        if (!timeline.enabled) {
            ++audit.blockers["preceding_extend_state_missing"];
            continue;
        }
        if (*timeline.enabled) {
            if (call.enabled) ++audit.retained_calls;
            else ++audit.required_insertions;
            if (!call.enabled || !timeline.active->source_readiness_topology_reused) bindings.emplace_back(&call, timeline.active);
            continue;
        }
        if (!call.enabled) {
            ++audit.retained_calls;
            continue;
        }
        if (!costs.contains({ call.logical_input, call.phase })) ++audit.blockers["inactive_call_sample_missing"];
        changes.push_back(&call);
    }
    if (!audit.blockers.empty()) {
        audit.status = "blocked";
        return audit;
    }
    std::map<std::string, std::vector<HiCacheLayerTransferTemplate>> layouts;
    std::set<HiCacheRewriteDecision *> attempted;
    std::vector<HiCacheLayerWaitInsertion> insertions;
    std::map<const HiCacheLayerWaitCall *, HiCacheLayerIo *> inserted_layers;
    for (const auto & [call, decision] : bindings) {
        const auto * fact = source.fact_node(decision->source_fact_node_id);
        if (decision->layer_io.empty() && !decision->source_readiness_topology_reused && attempted.insert(decision).second) {
            auto [sample, first] = layouts.try_emplace(fact->pid);
            if (first)
                for (const auto & donor : decisions) {
                    if (!donor.source_readiness_topology_reused || donor.effect_type != model::HiCacheEffectType::Loadback) continue;
                    const auto * donor_fact = source.fact_node(donor.source_fact_node_id);
                    if (!donor_fact || donor_fact->pid != fact->pid) continue;
                    sample->second = observe_hicache_layer_transfers(source, waits, donor);
                    if (!sample->second.empty()) break;
                }
            if (!expand_hicache_layer_io(sample->second, *decision, plan)) ++audit.blockers["source_layer_layout_or_target_io_missing"];
            else ++audit.expanded_loadbacks;
        }
        Ref ready;
        HiCacheLayerIo * layer = nullptr;
        if (!decision->layer_io.empty()) {
            const auto found = std::ranges::find(decision->layer_io, call->layer, &HiCacheLayerIo::layer);
            if (found == decision->layer_io.end()) {
                ++audit.blockers["target_layer_missing"];
                continue;
            }
            layer = &*found;
            ready = Ref::synthetic(layer->ready_id);
        }
        else if (decision->source_readiness_topology_reused) {
            std::set<size_t> records;
            for (const auto & observed : waits.calls)
                if (observed.enabled && observed.request_id == call->request_id && observed.layer == call->layer
                    && observed.logical_input == call->logical_input && observed.issue.empty())
                    records.insert(*observed.record);
            if (records.size() != 1) {
                ++audit.blockers["reused_layer_readiness_ambiguous"];
                continue;
            }
            ready = Ref::existing(*records.begin());
        }
        else continue;
        if (!call->enabled) {
            insertions.push_back({ call, ready });
            if (layer) inserted_layers.emplace(call, layer);
        }
        else {
            for (const auto id : source.incoming_edge_ids(*call->device_wait))
                if (graph.edge(id).active && graph.edge(id).kind == Kind::Sync) plan.disable_edges.push_back(id);
            plan.add_edges.push_back({ .src = ready,
                                       .dst = Ref::existing(*call->device_wait),
                                       .kind = Kind::Sync,
                                       .effect_id = effect,
                                       .reason = "retained call waits for its predicted loadback layer" });
            if (layer) layer->waits.push_back(Ref::existing(*call->device_wait));
            ++audit.rebound_calls;
        }
    }
    if (!audit.blockers.empty()) {
        audit.status = "blocked";
        return audit;
    }
    auto inserted = plan_hicache_layer_wait_insertions(source, waits, insertions);
    if (inserted.status != "ready") {
        audit.status = "blocked";
        audit.blockers = std::move(inserted.issues);
        return audit;
    }
    for (const auto & [call, layer] : inserted_layers) layer->waits.push_back(inserted.waits.at(call));
    for (const auto & change : inserted.mutation.set_cpu_gaps)
        if (std::ranges::any_of(plan.set_cpu_gaps, [&](const auto & old) { return old.node_id == change.node_id; }))
            ++audit.blockers["inserted_call_gap_already_modified"];
    if (!audit.blockers.empty()) {
        audit.status = "blocked";
        return audit;
    }
    plan.set_cpu_gaps.insert(plan.set_cpu_gaps.end(), inserted.mutation.set_cpu_gaps.begin(), inserted.mutation.set_cpu_gaps.end());
    plan.synthetic_nodes.insert(plan.synthetic_nodes.end(), inserted.mutation.synthetic_nodes.begin(), inserted.mutation.synthetic_nodes.end());
    plan.disable_edges.insert(plan.disable_edges.end(), inserted.mutation.disable_edges.begin(), inserted.mutation.disable_edges.end());
    plan.add_edges.insert(plan.add_edges.end(), inserted.mutation.add_edges.begin(), inserted.mutation.add_edges.end());
    audit.inserted_calls = inserted.inserted_calls;
    std::vector<HiCacheLayerWaitRemoval> removals;
    for (const auto * call : changes) {
        const Sample key{ call->logical_input, call->phase };
        auto & fractional = remainder[key];
        const auto total = fractional + costs.at(key);
        removals.push_back({ call, total / 1'000 });
        fractional = total % 1'000;
    }
    const auto removed = append_hicache_layer_wait_removals(source, removals, plan);
    audit.status = removed.status;
    audit.blockers.insert(removed.blockers.begin(), removed.blockers.end());
    audit.removed_calls = removed.removed_calls;
    audit.removed_main_cpu_us = removed.removed_main_cpu_us;
    audit.removed_worker_us = removed.removed_worker_us;
    audit.added_return_us = removed.added_return_us;
    return audit;
}

HiCacheLayerWaitPatch append_hicache_layer_wait_removals(const HiCacheSourceDagIndex & source,
                                                        std::span<const HiCacheLayerWaitRemoval> changes, core::DagMutationPlan & plan) {
    HiCacheLayerWaitPatch audit;
    const auto & graph = source.graph();
    core::DagMutationPlan addition{ .component = effect };
    std::map<size_t, uint64_t> gap_removal;
    std::map<size_t, core::DagGraph::CpuGapRanges> removed_ranges;
    std::set<size_t> workers, devices, submissions;
    for (const auto & change : changes) {
        const auto * call = change.call;
        if (!call || !call->enabled || !call->issue.empty() || !call->submission || !call->worker || !call->device_wait) {
            ++audit.blockers["removal_call_incomplete"];
            continue;
        }
        if (!submissions.insert(*call->submission).second) {
            ++audit.blockers["duplicate_removal_call"];
            continue;
        }
        const auto duration = change.return_us;
        addition.set_node_durations.push_back({ .node_id = *call->submission,
                                                .duration = duration,
                                                .effect_id = effect,
                                                .reason = "replace observed event submission with the supplied call return cost" });
        audit.added_return_us += duration;
        audit.removed_main_cpu_us += graph.node(*call->submission).duration;
        for (const auto & gap : call->cpu.owned_gap_slices) {
            gap_removal[gap.owner_node_id] += gap.owned_end_us - gap.owned_start_us;
            removed_ranges[gap.owner_node_id].emplace_back(gap.owned_start_us,gap.owned_end_us);
        }
        workers.insert(*call->worker);
        devices.insert(*call->device_wait);
    }
    for (const auto id : workers) audit.removed_worker_us += graph.node(id).duration;
    for (const auto & [id, removed] : gap_removal) {
        if (removed > graph.node(id).cpu_gap_after) {
            ++audit.blockers["call_gap_exceeds_source_gap"];
            continue;
        }
        if (std::ranges::any_of(plan.set_cpu_gaps, [&](const auto & item) { return item.node_id == id; })) {
            ++audit.blockers["call_gap_already_modified"];
            continue;
        }
        audit.removed_main_cpu_us += removed;
        std::optional<core::DagGraph::CpuGapRanges> retained;
        if (graph.node(id).cpu_gap_after == graph.node(id).original_cpu_gap_after) {
            retained.emplace();
            const auto& event = graph.event_for_node(id);
            auto cursor = event.ts+event.dur;
            const auto end = cursor+graph.node(id).original_cpu_gap_after;
            auto& cuts = removed_ranges.at(id);
            std::ranges::sort(cuts);
            for (const auto& [lo,hi] : cuts) {
                if (lo<cursor || hi<=lo || hi>end) {
                    ++audit.blockers["call_gap_ranges_overlap_or_exceed_source"];
                    break;
                }
                if (cursor<lo) retained->emplace_back(cursor,lo);
                cursor=hi;
            }
            if (cursor<end) retained->emplace_back(cursor,end);
        } else if (!graph.cpu_service_cost().empty()) {
            ++audit.blockers["call_gap_source_coordinates_already_modified"];
        }
        addition.set_cpu_gaps.push_back({ .node_id = id,
                                          .duration = graph.node(id).cpu_gap_after - removed,
                                          .effect_id = effect,
                                          .reason = "retain CPU gaps outside the removed call envelope",
                                          .retained_ranges = std::move(retained) });
    }
    std::set<size_t> disabled = workers;
    disabled.insert(devices.begin(), devices.end());
    for (const auto & update : plan.set_node_durations)
        if (disabled.contains(update.node_id) || submissions.contains(update.node_id)) ++audit.blockers["call_cost_already_modified"];
    if (!audit.blockers.empty()) {
        audit.status = "blocked";
        return audit;
    }
    bypass_device_waits(source, devices, addition);
    // Disabled infrastructure cannot define E2E; its deletion journal replaces
    // the old eligibility update, which would conflict in the same transaction.
    std::erase_if(plan.set_node_e2e_eligibility, [&](const auto & update) { return disabled.contains(update.node_id); });
    plan.set_node_durations.insert(plan.set_node_durations.end(), addition.set_node_durations.begin(), addition.set_node_durations.end());
    plan.set_cpu_gaps.insert(plan.set_cpu_gaps.end(), addition.set_cpu_gaps.begin(), addition.set_cpu_gaps.end());
    plan.disable_nodes.insert(plan.disable_nodes.end(), disabled.begin(), disabled.end());
    plan.add_edges.insert(plan.add_edges.end(), addition.add_edges.begin(), addition.add_edges.end());
    audit.removed_calls = changes.size();
    audit.status = "ready";
    return audit;
}

HiCacheLayerWaitPatch append_hicache_layer_wait_plan(const HiCacheSourceDagIndex & source, const HiCacheLayerWaitObservation & waits,
                                                     std::vector<HiCacheRewriteDecision> & decisions, core::DagMutationPlan & plan) {
    auto candidate = plan;
    auto effects = decisions;
    auto audit = build_layer_wait_plan(source, waits, effects, candidate);
    if (audit.status == "ready") {
        plan = std::move(candidate);
        decisions = std::move(effects);
    }
    return audit;
}
} // namespace markov::trace_graph::modules::hicache::patch
