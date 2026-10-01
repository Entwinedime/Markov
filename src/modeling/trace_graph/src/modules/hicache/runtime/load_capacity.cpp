#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/host_removal.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

std::vector<HiCacheLoadBranch> observe_load_branches(std::span<const core::TraceEvent> observations, std::span<const model::HiCacheReplayFact> facts,
                                                     uint64_t begin, uint64_t end) {
    const auto start = [](const auto & event) { return event.ts * 1'000 + event.ts_submicro_ns; };
    const auto stop = [&](const auto & event) { return start(event) + event.dur * 1'000 + event.dur_submicro_ns; };
    std::vector<const core::TraceEvent *> branches, checks;
    for (const auto & event : observations) {
        if (event.ts < begin || stop(event) > end * 1'000) continue;
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
        if (!condition || !used.insert(condition).second || condition->arg("request_id").empty() || condition->arg("status") != "returned"
            || (condition->arg("needed") != "true" && condition->arg("needed") != "false"))
            throw std::runtime_error("Load branch lacks a unique completed condition");
        const model::HiCacheReplayFact * owner = nullptr;
        bool ambiguous = false;
        for (const auto & item : facts) {
            const auto & fact = item.fact;
            if (item.role != HiCacheFactRole::CacheLookupInput || fact.pid != branch->pid || fact.tid != branch->tid
                || fact.request_id != condition->arg("request_id") || fact.source_ts * 1'000 > start(*branch))
                continue;
            if (!owner || owner->fact.source_ts < fact.source_ts) {
                owner = &item;
                ambiguous = false;
            }
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
    const patch::HiCacheSourceDagIndex source(graph);
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
        auto original = prepare_host_expansion(source, queues, host, "no host load: ");
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
        const auto positions = [&](const HiCacheHostExpansion & plan) { bind_call_resources(source, call, plan, true); };
        for (const auto & donor : load_admissions_)
            if (donor.pid == call.pid && donor.tid == call.tid) bind_load_resources(source, call, donor);
        for (const auto & [id, donor] : templates_)
            if (donor.pid == call.pid && donor.tid == call.tid) positions(donor.expansion);
        for (const auto & donor : eviction_controls_)
            if (donor.pid == call.pid && donor.tid == call.tid) positions(donor.expansion);
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
        empty_load_branches_.emplace(gate, std::move(branch));
        if (!capacity_calls_.emplace(call.owner, std::move(call)).second) throw std::runtime_error("Inactive load branch overlaps another capacity entry");
    }
}

void HiCacheWriteCalls::replace_active_load_branches(core::DagGraph & graph) {
    const patch::HiCacheSourceDagIndex source(graph);
    std::vector<const HiCacheLoadBranch *> branches;
    std::vector<HiCacheHostTemplate> hosts;
    std::set<size_t> owners;
    for (const auto & branch : load_branches_) {
        if (branch.condition.arg("needed") != "true") continue;
        // Base-derived clone primitives and independent primitives use the
        // same target branch replacement. Unsupported aggregate evidence does
        // not silently claim a new operation-count model.
        if (load_index_calibration_.empty() && !std::ranges::any_of(load_admissions_, [&](const auto & program) {
                return program.pid == branch.envelope.pid && program.tid == branch.envelope.tid && program.clone.has_value();
            }))
            continue;
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
    const patch::HiCacheSourceDagIndex retained(graph);
    for (const auto * branch : branches) {
        const auto key = std::pair{ branch->envelope.pid, branch->envelope.tid };
        const auto & program = load_admissions_.at(select_load_admission(key.first, key.second, 0));
        HiCacheHostExpansion empty;
        for (const auto & [node, candidate] : empty_load_branches_) {
            const auto & fact = replay_.fact(candidate.owner).fact;
            if (fact.pid == key.first && fact.tid == key.second) {
                empty = candidate.original;
                break;
            }
        }
        if (empty.nodes.empty()) {
            // No inactive source branch was measured. The existing proxy is
            // CPU bookkeeping only, not the successful branch's concatenation.
            double cpu = 0, residual = 0;
            for (const auto & part : program.tail.work.nodes) {
                if (!part.work.is_cpu || part.queue_member) continue;
                cpu += part.work.duration;
                residual += part.work.cpu_gap_after;
            }
            std::pair<double, double> remainder{};
            empty = generated_cpu_control(graph.node_lane_key(branch->entry_node), cpu, residual, remainder, "target no-load branch: bookkeeping cost proxy");
        }
        empty_load_branches_.emplace(branch->entry_node, EmptyLoadBranch{ branch->owner, branch->return_node, std::move(empty) });
        const auto & event = graph.event_for_node(branch->entry_node);
        Call call{ .owner = branch->owner,
                   .pid = key.first,
                   .tid = key.second,
                   .at_us = event.ts,
                   .entry_node = branch->entry_node,
                   .lane = std::string(graph.node_lane_key(branch->entry_node)) };
        const auto positions = [&](const HiCacheHostExpansion & plan) { bind_call_resources(retained, call, plan, true); };
        for (const auto & donor : load_admissions_)
            if (donor.pid == call.pid && donor.tid == call.tid) bind_load_resources(retained, call, donor);
        for (const auto & [id, donor] : templates_)
            if (donor.pid == call.pid && donor.tid == call.tid) positions(donor.expansion);
        for (const auto & donor : eviction_controls_)
            if (donor.pid == call.pid && donor.tid == call.tid) positions(donor.expansion);
        capacity_calls_.emplace(branch->owner, std::move(call));
    }
}

void HiCacheWriteCalls::rebind_retained_resources(const patch::HiCacheSourceDagIndex & retained, const std::map<size_t, size_t> & members) {
    // Bind after all source regions have been replaced, including load
    // submissions whose compute Records can be the next stream endpoint.
    const auto & graph = retained.graph();
    const auto refresh = [&](Call & call) { rebind_host_stream_positions(retained, call.positions, call.pid, call.tid, call.at_us); };
    for (auto & call : calls_)
        if (graph.node(call.entry_node).active) refresh(call);
    for (auto & [owner, call] : capacity_calls_) refresh(call);
    for (auto & [node, calls] : lifecycle_writes_at_)
        for (auto & call : calls) refresh(call);
    auto plans = host_plans();
    rebind_host_worker_queues(graph, members, plans);
    writes_.rebind_consumers(retained);
}

void HiCacheWriteCalls::prepare_load_capacity(const patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues) {
    struct Failure {
        uint64_t duration;
        HiCacheHostExpansion expansion;
    };
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
        if (attempts.size() > 2
            || (attempts.size() == 2
                && (attempts[0]->event.arg("allocated") != "false" || attempts[0]->event.ts + attempts[0]->event.dur > attempts[1]->event.ts
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
