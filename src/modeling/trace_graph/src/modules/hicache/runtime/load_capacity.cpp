#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/host_removal.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {
void HiCacheWriteCalls::bind_load_branches(core::DagGraph & graph, uint64_t begin, uint64_t end) {
    load_branches_ = source_dag::observe_load_branches(graph.runtime_observations(), replay_.facts(), begin, end);
    std::vector<core::TraceEvent> points;
    for (const auto & branch : load_branches_) {
        for (const auto * event : { &branch.condition, &branch.envelope }) {
            core::TraceEvent point;
            point.name = "hicache.load_branch_boundary";
            point.pid = event->pid;
            point.tid = event->tid;
            point.ts = (event->ts * 1'000 + event->ts_submicro_ns + event->dur * 1'000 + event->dur_submicro_ns) / 1'000;
            points.push_back(std::move(point));
        }
    }
    const auto bound = bind_hicache_control_points(graph, points);
    for (size_t i = 0; i < load_branches_.size(); ++i) {
        if (!bound[2 * i] || !bound[2 * i + 1]) throw std::runtime_error("Load branch lacks exact CPU tail boundaries");
        load_branches_[i].entry_node = *bound[2 * i];
        load_branches_[i].return_node = *bound[2 * i + 1];
    }
}

void HiCacheWriteCalls::bind_load_attempts(core::DagGraph & graph, uint64_t begin, uint64_t end) {
    const adapters::sglang::HiCacheSourceIndex source(graph);
    std::vector<core::TraceEvent> points;
    for (const auto & event : graph.runtime_observations()) {
        if (event.name != "runtime.hicache.load_allocation" || event.ts < begin || event.ts + event.dur > end) continue;
        if (event.arg("status") != "returned" || !event.dur || (event.arg("allocated") != "true" && event.arg("allocated") != "false"))
            throw std::runtime_error("Load attempt lacks its completed allocation result");
        const auto owner = observe_write_call_owner(source, replay_.facts(), event.pid, event.tid, event.ts, event.ts + event.dur);
        if (replay_.fact(owner).role != HiCacheFactRole::CacheLookupInput) throw std::runtime_error("Load attempt is outside a lookup admission");
        load_attempts_.push_back({ owner, 0, event });
        for (const auto time : { event.ts, event.ts + event.dur }) {
            core::TraceEvent point;
            point.name = "hicache.load_attempt";
            point.pid = event.pid;
            point.tid = event.tid;
            point.ts = time;
            points.push_back(std::move(point));
        }
    }
    const auto bound = bind_hicache_control_points(graph, points);
    const adapters::sglang::HiCacheSourceIndex bounded(graph);
    core::DagMutationPlan entries{ .component = "hicache_load_allocation" };
    for (size_t i = 0; i < load_attempts_.size(); ++i) {
        if (!bound[2 * i] || !bound[2 * i + 1]) throw std::runtime_error("Load attempt lacks exact entry/return boundaries");
        const auto first = *bound[2 * i];
        // Keep the original entry's work and gap after the new branch. A
        // callback on that entry itself could overlap them with the eviction.
        const auto & event = load_attempts_[i].event;
        (void)append_host_entry_gate(bounded,
                                     first,
                                     { event.pid, event.tid, event.ts, graph.node(first).gpu_id },
                                     "allocation_entry:" + std::to_string(i),
                                     "load allocation entry",
                                     entries);
    }

    const auto applied = core::apply_dag_mutation_plan(graph, entries);
    for (size_t i = 0; i < load_attempts_.size(); ++i) {
        load_attempts_[i].entry_node = applied.synthetic_node_ids.at("allocation_entry:" + std::to_string(i));
        load_attempts_[i].return_node = *bound[2 * i + 1];
    }
}

void HiCacheWriteCalls::prepare_empty_load_branches(core::DagGraph & graph) {
    const adapters::sglang::HiCacheSourceIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    std::vector<std::pair<const HiCacheLoadBranch *, HiCacheHostTemplate>> hosts;
    for (const auto & branch : load_branches_) {
        if (branch.condition.arg("needed") != "false") continue;
        const auto end_us = [](const auto & e) { return (e.ts * 1'000 + e.ts_submicro_ns + e.dur * 1'000 + e.dur_submicro_ns) / 1'000; };
        hosts.emplace_back(&branch, observe_host_template(source, branch.envelope.pid, branch.envelope.tid, end_us(branch.condition), end_us(branch.envelope)));
    }
    core::DagMutationPlan replacement{ .component = "hicache_load_admission" };
    std::map<size_t, uint64_t> remaining_gaps;
    std::map<std::string, std::pair<EmptyLoadBranch, Call>> pending;
    for (const auto & [branch, host] : hosts) {
        auto original = prepare_host_expansion(source, queues, host, "no host load: ", { .compact = compact_programs_ });
        if (!host.worker_nodes.empty() || !original.streams.empty() || !original.waits.empty() || !original.event_waits.empty())
            throw std::runtime_error("Inactive host-load branch contains device or worker work");
        const auto first = branch->entry_node;
        const auto event = graph.event_for_node(first);
        const auto gate = append_host_entry_gate(source,
                                                 first,
                                                 { event.pid, event.tid, event.ts, graph.node(first).gpu_id },
                                                 "branch_entry:" + std::to_string(first),
                                                 "host load branch entry",
                                                 replacement);
        Call call{ .owner = branch->owner, .pid = event.pid, .tid = event.tid, .at_us = event.ts };
        call.lane = std::string(graph.node_lane_key(first));
        for (const auto & donor : load_admissions_)
            if (donor.pid == call.pid && donor.tid == call.tid) bind_load_resources(source, call, donor);
        for (const auto & [id, donor] : observed_write_fragments_)
            if (donor.pid == call.pid && donor.tid == call.tid) bind_call_resources(source, call, donor.resources, true);
        for (const auto & donor : observed_eviction_fragments_)
            if (donor.pid == call.pid && donor.tid == call.tid) bind_call_resources(source, call, donor.resources, true);
        pending.emplace(gate.synthetic_id,
                        std::pair{
                            EmptyLoadBranch{ branch->owner, first, std::move(original) },
                            std::move(call)
        });
        for (const auto id : host.main.owned_node_ids) replacement.set_node_durations.push_back({ id, 0 });
        for (const auto & gap : host.main.owned_gap_slices) {
            auto & retained = remaining_gaps.try_emplace(gap.owner_node_id, graph.node(gap.owner_node_id).cpu_gap_after).first->second;
            const auto owned = gap.owned_duration_us();
            if (retained < owned) throw std::runtime_error("Inactive branch gap ownership exceeds its retained interval");
            retained -= owned;
        }
    }
    for (const auto & [node, duration] : remaining_gaps) replacement.set_cpu_gaps.push_back({ node, duration });

    const auto applied = core::apply_dag_mutation_plan(graph, replacement);
    for (auto & [id, entry] : pending) {
        auto & [branch, call] = entry;
        const auto gate = applied.synthetic_node_ids.at(id);
        call.entry_node = gate;
        graph.mutable_node(gate).observe_start = true;
        empty_load_branches_.emplace(gate, std::move(branch));
        if (!capacity_calls_.emplace(call.owner, std::move(call)).second) throw std::runtime_error("Inactive load branch overlaps another capacity entry");
    }
}

void HiCacheWriteCalls::replace_active_load_branches(core::DagGraph & graph) {
    const adapters::sglang::HiCacheSourceIndex source(graph);
    std::vector<const HiCacheLoadBranch *> branches;
    std::vector<HiCacheHostTemplate> hosts;
    std::set<size_t> owners;
    for (const auto & branch : load_branches_) {
        if (branch.condition.arg("needed") != "true") continue;
        // Every observed active branch is replaced by target rules. Missing
        // primitive costs are reported when that work executes, never used to
        // retain the source branch as a hidden prediction fallback.
        branches.push_back(&branch);
        owners.insert(branch.owner);
        const auto & first = graph.event_for_node(branch.entry_node);
        const auto & last = graph.event_for_node(branch.return_node);
        hosts.push_back(observe_host_template(source, first.pid, first.tid, first.ts, last.ts));
    }
    std::vector<HiCacheHostRegion> regions;
    for (size_t i = 0; i < branches.size(); ++i) regions.push_back({ branches[i]->entry_node, branches[i]->return_node, &hosts[i] });
    (void)core::apply_dag_mutation_plan(graph, plan_host_removal(source, regions));
    // The whole admission now owns any nested eviction. Old callbacks must
    // not publish an allocation or submit the same target victim a second time.
    for (auto & [node, actions] : actions_) std::erase_if(actions, [&](const auto & action) { return owners.contains(calls_.at(action.second).owner); });
    std::erase_if(actions_, [&](const auto & entry) { return entry.second.empty() || !graph.node(entry.first).active; });
    for (auto & [node, allocations] : allocation_returns_) std::erase_if(allocations, [&](size_t owner) { return owners.contains(owner); });
    std::erase_if(allocation_returns_, [&](const auto & entry) { return entry.second.empty() || !graph.node(entry.first).active; });
    std::erase_if(capacity_at_, [&](const auto & entry) { return owners.contains(entry.second); });
    for (const auto owner : owners) {
        source_owners_.erase(owner);
        wait_owners_.erase(owner);
        capacity_calls_.erase(owner);
        capacity_position_errors_.erase(owner);
        load_failure_templates_.erase(owner);
    }
    const adapters::sglang::HiCacheSourceIndex retained(graph);
    for (const auto * branch : branches) {
        const auto key = std::pair{ branch->envelope.pid, branch->envelope.tid };
        HiCacheHostExpansion empty;
        // This request did not observe the inactive branch. Keep its cost
        // unbound until target state selects it; another request is not a
        // corresponding source operation and cannot donate its envelope.
        graph.mutable_node(branch->entry_node).observe_start = true;
        empty_load_branches_.emplace(branch->entry_node, EmptyLoadBranch{ branch->owner, branch->return_node, std::move(empty) });
        const auto & event = graph.event_for_node(branch->entry_node);
        Call call{ .owner = branch->owner,
                   .pid = key.first,
                   .tid = key.second,
                   .at_us = event.ts,
                   .entry_node = branch->entry_node,
                   .lane = std::string(graph.node_lane_key(branch->entry_node)) };
        for (const auto & donor : load_admissions_)
            if (donor.pid == call.pid && donor.tid == call.tid) bind_load_resources(retained, call, donor);
        for (const auto & [id, donor] : observed_write_fragments_)
            if (donor.pid == call.pid && donor.tid == call.tid) bind_call_resources(retained, call, donor.resources, true);
        for (const auto & donor : observed_eviction_fragments_)
            if (donor.pid == call.pid && donor.tid == call.tid) bind_call_resources(retained, call, donor.resources, true);
        capacity_calls_.emplace(branch->owner, std::move(call));
    }
}

void HiCacheWriteCalls::rebind_retained_resources(const adapters::sglang::HiCacheSourceIndex & retained, const std::map<size_t, size_t> & members) {
    // Bind after all source regions have been replaced, including load
    // submissions whose compute Records can be the next stream endpoint.
    const auto & graph = retained.graph();
    const auto refresh = [&](Call & call) {
        rebind_host_stream_positions(retained, call.positions, call.pid, call.tid, call.at_us);

        // Synthetic insertion sites captured their outgoing continuation
        // before phase/allocator replacement. Reconnect to the surviving
        // boundary now, not to an inactive payload node saved during bind.
        // Replaced source calls and eviction envelopes instead keep their
        // explicit return gates; those are not immediate-successor snapshots.
        if (call.source || call.retained_allocation_return) return;
        call.successors.clear();
        for (const auto edge_id : retained.outgoing_edge_ids(call.entry_node)) {
            const auto & edge = graph.edge(edge_id);
            if (edge.active && graph.node(edge.dst).active) call.successors.push_back(edge.dst);
        }
    };
    for (auto & call : calls_)
        if (graph.node(call.entry_node).active) refresh(call);
    for (auto & [owner, call] : capacity_calls_) {
        // A target eviction may write even when the source only released
        // pages. Bind the generated transfer resource too, after all source
        // removals; observed eviction fragments cannot enumerate that lane.
        if (const auto resources = write_resources_.find({ call.pid, call.tid }); resources != write_resources_.end()) {
            for (const auto resource : { resources->second.compute, resources->second.transfer }) {
                const auto lane = graph.node(resource).lane_id;
                if (call.positions.contains(lane)) continue;

                try {
                    call.positions.emplace(lane, observe_write_stream_position(retained, resource, call.pid, call.tid, call.at_us));
                }
                catch (const std::runtime_error & error) {
                    // Unused resources need not have an observable cut. Keep
                    // the failure for the target branch that actually needs it.
                    capacity_position_errors_[owner].insert_or_assign(lane, error.what());
                }
            }
        }
        refresh(call);
    }
    for (auto & [node, calls] : lifecycle_writes_at_)
        for (auto & call : calls) refresh(call);
    auto plans = host_plans();
    rebind_host_worker_queues(graph, members, plans);
    for (auto & [key, resources] : write_resources_) {
        const auto lane = graph.node(resources.worker).lane_id;
        resources.worker = members.at(lane);
    }
}


} // namespace markov::trace_graph::modules::hicache::runtime
