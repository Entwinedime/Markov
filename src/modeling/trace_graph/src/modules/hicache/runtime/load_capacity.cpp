#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/host_removal.hpp"
#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

std::vector<HiCacheLoadBranch> observe_load_branches(
    std::span<const core::TraceEvent> observations, std::span<const model::HiCacheReplayFact> facts, uint64_t begin, uint64_t end) {
    const auto start = [](const auto & event) { return event.ts * 1000 + event.ts_submicro_ns; };
    const auto stop = [&](const auto & event) { return start(event) + event.dur * 1000 + event.dur_submicro_ns; };
    std::vector<const core::TraceEvent *> branches, checks;
    for (const auto & event : observations) {
        if (event.ts < begin || stop(event) > end * 1000) continue;
        if (event.name == "hicache.control.host_load_branch") branches.push_back(&event);
        if (event.name == "runtime.hicache.host_load_check") checks.push_back(&event);
    }
    std::vector<HiCacheLoadBranch> result;
    std::set<const core::TraceEvent *> used;
    for (const auto * branch : branches) {
        const core::TraceEvent * condition = nullptr;
        for (const auto * check : checks) {
            if (check->pid != branch->pid || check->tid != branch->tid || start(*check) < start(*branch) || stop(*check) > stop(*branch)) continue;
            if (condition) throw std::runtime_error("Load branch has multiple conditions");
            condition = check;
        }
        if (!condition || !used.insert(condition).second || condition->arg("request_id").empty()
            || condition->arg("status") != "returned" || (condition->arg("needed") != "true" && condition->arg("needed") != "false"))
            throw std::runtime_error("Load branch lacks a unique completed condition");
        const model::HiCacheReplayFact * owner = nullptr;
        bool ambiguous = false;
        for (const auto & item : facts) {
            const auto & fact = item.fact;
            if (item.role != HiCacheFactRole::CacheLookupInput || fact.pid != branch->pid || fact.tid != branch->tid
                || fact.request_id != condition->arg("request_id") || fact.source_ts * 1000 > start(*branch)) continue;
            if (!owner || owner->fact.source_ts < fact.source_ts) { owner = &item; ambiguous = false; }
            else if (owner->fact.source_ts == fact.source_ts) ambiguous = true;
        }
        if (ambiguous) throw std::runtime_error("Load branch has ambiguous lookup inputs");
        if (!owner) throw std::runtime_error("Load branch lacks a preceding lookup of the same request and thread");
        result.push_back({ owner->fact.source_node_id, *branch, *condition });
    }
    if (used.size() != checks.size()) throw std::runtime_error("Load condition lacks its complete outer branch");
    return result;
}

void HiCacheWriteCalls::bind_load_branches(core::DagGraph & graph, uint64_t begin, uint64_t end) {
    load_branches_ = observe_load_branches(graph.runtime_observations(), replay_.facts(), begin, end);
    std::vector<core::TraceEvent> points;
    for (const auto & branch : load_branches_) {
        for (const auto * event : { &branch.condition, &branch.envelope }) {
            core::TraceEvent point;
            point.name = "hicache.load_branch_boundary";
            point.pid = event->pid; point.tid = event->tid;
            point.ts = (event->ts * 1000 + event->ts_submicro_ns + event->dur * 1000 + event->dur_submicro_ns) / 1000;
            points.push_back(std::move(point));
        }
    }
    const auto bound = bind_hicache_control_points(graph, points);
    for (size_t i = 0; i < load_branches_.size(); ++i) {
        if (!bound[2*i] || !bound[2*i+1]) throw std::runtime_error("Load branch lacks exact CPU tail boundaries");
        load_branches_[i].entry_node = *bound[2*i];
        load_branches_[i].return_node = *bound[2*i+1];
    }
}

void HiCacheWriteCalls::bind_load_attempts(core::DagGraph & graph, uint64_t begin, uint64_t end) {
    const patch::HiCacheSourceDagIndex source(graph);
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
    const patch::HiCacheSourceDagIndex bounded(graph);
    for (size_t i = 0; i < load_attempts_.size(); ++i) {
        if (!bound[2 * i] || !bound[2 * i + 1]) throw std::runtime_error("Load attempt lacks exact entry/return boundaries");
        const auto first = *bound[2 * i];
        // Keep the original entry's work and gap after the new branch. A
        // callback on that entry itself could overlap them with the eviction.
        const auto gate = graph.add_synthetic_node({ .name = "load allocation entry", .category = "execution_gate",
                                                     .lane_key = std::string(graph.node_lane_key(first)),
                                                     .observed_point = core::DagObservedPoint{ load_attempts_[i].event.pid, load_attempts_[i].event.tid,
                                                                                              load_attempts_[i].event.ts, graph.node(first).gpu_id } });
        for (const auto edge : bounded.incoming_edge_ids(first))
            if (graph.edge(edge).active && graph.edge(edge).dst == first) graph.mutable_edge(edge).dst = gate;
        graph.add_edge(gate, first, core::DagEdgeKind::Sequential);
        load_attempts_[i].entry_node = gate;
        load_attempts_[i].return_node = *bound[2 * i + 1];
    }
}

void HiCacheWriteCalls::prepare_empty_load_branches(core::DagGraph & graph) {
    const patch::HiCacheSourceDagIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    std::vector<std::pair<const HiCacheLoadBranch *, HiCacheHostTemplate>> hosts;
    for (const auto & branch : load_branches_) {
        if (branch.condition.arg("needed") != "false") continue;
        const auto end_us = [](const auto & e) { return (e.ts * 1000 + e.ts_submicro_ns + e.dur * 1000 + e.dur_submicro_ns) / 1000; };
        hosts.emplace_back(&branch, observe_host_template(source, branch.envelope.pid, branch.envelope.tid,
                                                          end_us(branch.condition), end_us(branch.envelope)));
    }
    for (const auto & [branch, host] : hosts) {
        auto original = prepare_host_expansion(source, queues, host, "no host load: ");
        if (!host.worker_nodes.empty() || !original.streams.empty() || !original.waits.empty() || !original.event_waits.empty())
            throw std::runtime_error("Inactive host-load branch contains device or worker work");
        const auto first = branch->entry_node;
        const auto event = graph.event_for_node(first);
        const auto gate = graph.add_synthetic_node({ .name = "host load branch entry", .category = "execution_gate",
            .lane_key = std::string(graph.node_lane_key(first)),
            .observed_point = core::DagObservedPoint{event.pid, event.tid, event.ts, graph.node(first).gpu_id} });
        for (const auto id : source.incoming_edge_ids(first))
            if (graph.edge(id).active && graph.edge(id).dst == first) graph.mutable_edge(id).dst = gate;
        graph.add_edge(gate, first, core::DagEdgeKind::Sequential);
        empty_load_branches_.emplace(gate, EmptyLoadBranch{branch->owner, first, std::move(original)});
        Call call{ .owner = branch->owner, .pid = event.pid, .tid = event.tid, .at_us = event.ts, .entry_node = gate };
        call.lane = std::string(graph.node_lane_key(first));
        const auto positions = [&](const HiCacheHostExpansion & plan) {
            const auto observe = [&](size_t sample) {
                const auto lane = graph.node(sample).lane_id;
                if (call.positions.contains(lane) || capacity_position_errors_[call.owner].contains(lane)) return;
                try { call.positions.emplace(lane, observe_write_stream_position(source, sample, call.pid, call.tid, call.at_us)); }
                catch (const std::runtime_error & error) { capacity_position_errors_[call.owner].emplace(lane, error.what()); }
            };
            for (const auto & stream : plan.streams) observe(stream.source_node);
            for (const auto & wait : plan.waits) observe(wait.source_node);
        };
        for (const auto & donor : load_admissions_)
            if (donor.pid == call.pid && donor.tid == call.tid) {
                positions(donor.prefix); positions(donor.allocation); positions(donor.suffix);
                if (donor.retry) positions(*donor.retry);
            }
        if (const auto at = generated_load_admissions_.find({call.pid, call.tid}); at != generated_load_admissions_.end()) {
            for (const auto & plan : at->second.allocation) positions(plan);
            positions(at->second.clone);
        }
        for (const auto & [id, donor] : templates_)
            if (donor.pid == call.pid && donor.tid == call.tid) positions(donor.expansion);
        for (const auto & donor : eviction_controls_)
            if (donor.pid == call.pid && donor.tid == call.tid) positions(donor.expansion);
        if (!capacity_calls_.emplace(call.owner, std::move(call)).second)
            throw std::runtime_error("Inactive load branch overlaps another capacity entry");
        for (const auto id : host.main.owned_node_ids) graph.set_node_duration(id, 0);
        for (const auto & gap : host.main.owned_gap_slices) {
            auto & retained = graph.mutable_node(gap.owner_node_id).cpu_gap_after;
            const auto owned = gap.owned_duration_us();
            if (retained < owned) throw std::runtime_error("Inactive branch gap ownership exceeds its retained interval");
            retained -= owned;
        }
    }
}

void HiCacheWriteCalls::replace_active_load_branches(core::DagGraph & graph) {
    if (load_index_calibration_.empty()) return;
    const patch::HiCacheSourceDagIndex source(graph);
    std::vector<const HiCacheLoadBranch *> branches;
    std::vector<HiCacheHostTemplate> hosts;
    std::set<size_t> owners;
    for (const auto & branch : load_branches_) {
        if (branch.condition.arg("needed") != "true") continue;
        branches.push_back(&branch);
        owners.insert(branch.owner);
        const auto & first = graph.event_for_node(branch.entry_node);
        const auto & last = graph.event_for_node(branch.return_node);
        hosts.push_back(observe_host_template(source, first.pid, first.tid, first.ts, last.ts));
    }
    std::vector<HiCacheHostRegion> regions;
    for (size_t i = 0; i < branches.size(); ++i)
        regions.push_back({branches[i]->entry_node, branches[i]->return_node, &hosts[i]});
    (void)core::apply_dag_mutation_plan(graph, plan_host_removal(source, regions));
    // The whole admission now owns any nested eviction. Old callbacks must
    // not publish an allocation or submit the same target victim a second time.
    for (auto & [node, actions] : actions_)
        std::erase_if(actions, [&](const auto & action) { return owners.contains(calls_.at(action.second).owner); });
    std::erase_if(actions_, [&](const auto & entry) { return entry.second.empty() || !graph.node(entry.first).active; });
    for (auto & [node, allocations] : allocation_returns_)
        std::erase_if(allocations, [&](size_t owner) { return owners.contains(owner); });
    std::erase_if(allocation_returns_, [&](const auto & entry) { return entry.second.empty() || !graph.node(entry.first).active; });
    std::erase_if(capacity_at_, [&](const auto & entry) { return owners.contains(entry.second); });
    for (const auto owner : owners) {
        source_owners_.erase(owner); wait_owners_.erase(owner);
        capacity_calls_.erase(owner); capacity_position_errors_.erase(owner);
        load_failure_templates_.erase(owner);
    }
    const patch::HiCacheSourceDagIndex retained(graph);
    for (const auto * branch : branches) {
        const auto key = std::pair{branch->envelope.pid, branch->envelope.tid};
        const auto program = generated_load_admissions_.find(key);
        if (program == generated_load_admissions_.end())
            throw std::runtime_error("Active load admission lacks independent costs or scheduler resource evidence");
        HiCacheHostExpansion empty;
        for (const auto & [node, candidate] : empty_load_branches_) {
            const auto & fact = replay_.fact(candidate.owner).fact;
            if (fact.pid == key.first && fact.tid == key.second) { empty = candidate.original; break; }
        }
        if (empty.nodes.empty()) {
            empty = program->second.tail;
            for (auto & part : empty.nodes) part.work.name = "target no-load branch: independent bookkeeping cost proxy";
        }
        empty_load_branches_.emplace(branch->entry_node, EmptyLoadBranch{branch->owner, branch->return_node, std::move(empty)});
        const auto & event = graph.event_for_node(branch->entry_node);
        Call call{.owner = branch->owner, .pid = key.first, .tid = key.second, .at_us = event.ts,
                  .entry_node = branch->entry_node, .lane = std::string(graph.node_lane_key(branch->entry_node))};
        const auto positions = [&](const HiCacheHostExpansion & plan) {
            const auto observe = [&](size_t sample) {
                const auto lane = graph.node(sample).lane_id;
                if (call.positions.contains(lane) || capacity_position_errors_[call.owner].contains(lane)) return;
                try { call.positions.emplace(lane, observe_write_stream_position(retained, sample, call.pid, call.tid, call.at_us)); }
                catch (const std::runtime_error & error) { capacity_position_errors_[call.owner].emplace(lane, error.what()); }
            };
            for (const auto & stream : plan.streams) observe(stream.source_node);
            for (const auto & wait : plan.waits) observe(wait.source_node);
        };
        for (const auto & plan : program->second.allocation) positions(plan);
        positions(program->second.clone);
        for (const auto & [id, donor] : templates_)
            if (donor.pid == call.pid && donor.tid == call.tid) positions(donor.expansion);
        for (const auto & donor : eviction_controls_)
            if (donor.pid == call.pid && donor.tid == call.tid) positions(donor.expansion);
        capacity_calls_.emplace(branch->owner, std::move(call));
    }
}

void HiCacheWriteCalls::rebind_retained_resources(core::DagGraph & graph) {
    // Bind after all source regions have been replaced, including load
    // submissions whose compute Records can be the next stream endpoint.
    const patch::HiCacheSourceDagIndex retained(graph);
    const auto refresh = [&](Call & call) {
        for (auto & [lane, position] : call.positions) {
            if ((!position.before || graph.node(*position.before).active)
                && (!position.after || graph.node(*position.after).active)) continue;
            const auto sample = position.before ? *position.before : *position.after;
            position = observe_write_stream_position(retained, sample, call.pid, call.tid, call.at_us);
        }
    };
    for (auto & call : calls_) if (graph.node(call.entry_node).active) refresh(call);
    for (auto & [owner, call] : capacity_calls_) refresh(call);
    for (auto & [node, calls] : lifecycle_writes_at_) for (auto & call : calls) refresh(call);
    std::vector<HiCacheHostExpansion *> plans;
    for (auto & [id, donor] : templates_) plans.push_back(&donor.expansion);
    for (auto & donor : eviction_controls_) plans.push_back(&donor.expansion);
    for (auto & [id, plan] : load_failure_templates_) plans.push_back(&plan);
    for (auto & [key, program] : generated_load_admissions_) {
        for (auto & plan : program.allocation) plans.push_back(&plan);
        plans.push_back(&program.clone);
    }
    rebind_host_worker_queues(graph, plans);
}

void HiCacheWriteCalls::prepare_load_capacity(const patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues) {
    struct Failure { uint64_t duration; HiCacheHostExpansion expansion; };
    std::map<std::pair<std::string, std::string>, std::vector<Failure>> failures;
    std::map<size_t, std::vector<const LoadAttempt *>> grouped;
    for (const auto & attempt : load_attempts_) {
        grouped[attempt.owner].push_back(&attempt);
        const auto & event = attempt.event;
        if (event.arg("allocated") != "false") continue;
        const auto host = observe_host_template(source, event.pid, event.tid, event.ts, event.ts + event.dur);
        auto expansion = prepare_host_expansion(source, queues, host, "failed load allocation: ");
        // A failed allocation can sort free pages on other allocator paths.
        // Such a donor needs device-lane binding; do not treat it as pure CPU.
        if (!host.worker_nodes.empty() || !expansion.streams.empty() || !expansion.waits.empty() || !expansion.event_waits.empty()) continue;
        failures[{ event.pid, event.tid }].push_back({ event.dur, std::move(expansion) });
    }
    for (auto & [lane, samples] : failures) std::ranges::stable_sort(samples, {}, &Failure::duration);
    for (auto & [owner, attempts] : grouped) {
        std::ranges::sort(attempts, {}, [](const auto * item) { return item->event.ts; });
        if (attempts.size() > 2 || (attempts.size() == 2 && (attempts[0]->event.arg("allocated") != "false"
            || attempts[0]->event.ts + attempts[0]->event.dur > attempts[1]->event.ts
            || attempts[0]->event.arg("node_id") != attempts[1]->event.arg("node_id"))))
            throw std::runtime_error("Load admission has an unsupported attempt sequence");
        // Existing source eviction is still owned by its observed write calls.
        if (attempts.size() != 1 || attempts.front()->event.arg("allocated") != "true") continue;
        const auto & attempt = *attempts.front();
        const auto & event = attempt.event;
        const auto samples = failures.find({ event.pid, event.tid });
        if (samples == failures.end()) continue;
        if (guarded_owners_.contains(owner)) throw std::runtime_error("Lookup has multiple capacity opportunities");
        load_failure_templates_.emplace(owner, samples->second[samples->second.size() / 2].expansion);
        // Insert before the original successful allocation, never after it.
        // On the target branch this original call becomes the successful retry.
        capacity_guards_.push_back({ owner, event.pid, event.tid, event.ts, event.ts, attempt.entry_node, attempt.entry_node });
        guarded_owners_.insert(owner);
        allocation_returns_[attempt.entry_node].push_back(owner);
    }
}

} // namespace markov::trace_graph::modules::hicache::runtime
