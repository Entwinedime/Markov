#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {
const HiCacheWriteCalls::WriteTemplate & HiCacheWriteCalls::select_write_template(const std::string & pid, const std::string & tid, uint64_t bytes) const {
    const WriteTemplate * selected = nullptr;
    uint64_t nearest = 0;
    for (const auto & [id, candidate] : templates_) {
        if (candidate.pid != pid || candidate.tid != tid || candidate.write_back != std::optional<bool>{ write_back_ }
            || !write_page_geometry_matches(candidate.expansion, bytes, page_bytes_))
            continue;
        const auto source_bytes = candidate.expansion.payload_bytes;
        const auto distance = source_bytes > bytes ? source_bytes - bytes : bytes - source_bytes;
        // Compatible base geometry can be extrapolated before consulting an
        // independent measurement, even when that measurement is closer in size.
        if (!selected || (selected->independent && !candidate.independent) || (selected->independent == candidate.independent && distance < nearest)) {
            selected = &candidate;
            nearest = distance;
        }
    }
    if (!selected) throw std::runtime_error("Target write lacks a same-thread, same-policy template with projectable page geometry");
    return *selected;
}

size_t observe_write_call_owner(const patch::HiCacheSourceDagIndex & source, std::span<const model::HiCacheReplayFact> facts, const std::string & pid,
                                const std::string & tid, uint64_t begin, uint64_t end) {
    const model::HiCacheReplayFact * owner = nullptr;
    for (const auto & item : facts) {
        const auto & fact = item.fact;
        if (fact.pid != pid || fact.tid != tid || !fact.dur) continue;
        if (item.role != HiCacheFactRole::CacheLifecycleCommit && item.role != HiCacheFactRole::CacheDecodeAllocation) continue;
        const auto start = item.role == HiCacheFactRole::CacheLifecycleCommit ? fact.source_ts - fact.dur : fact.source_ts;
        if (start <= begin && start + fact.dur >= end) {
            if (owner) throw std::runtime_error("Write call has multiple enclosing state actions");
            owner = &item;
        }
    }
    if (owner) return owner->fact.source_node_id;
    // init_load_back carries request identity and encloses allocation/eviction.
    // Its source hit counts and output pages are deliberately not consulted.
    const patch::HiCacheSourceFactNode * admission = nullptr;
    for (const auto & fact : source.fact_nodes()) {
        if (fact.fact_role != "loadback_decision_observed" || fact.phase != "end" || fact.pid != pid || fact.tid != tid) continue;
        if (fact.timestamp_us <= begin && fact.timestamp_us + fact.duration_us >= end) {
            if (admission) throw std::runtime_error("Write call has multiple loadback admission envelopes");
            admission = &fact;
        }
    }
    if (admission) {
        for (const auto & item : facts) {
            const auto & fact = item.fact;
            if (item.role != HiCacheFactRole::CacheLookupInput || fact.pid != pid || fact.tid != tid || fact.request_id != admission->request_id
                || fact.cache_scope != admission->cache_scope || fact.source_ts > admission->timestamp_us)
                continue;
            if (!owner || fact.source_ts > owner->fact.source_ts) owner = &item;
        }
        if (!owner) throw std::runtime_error("Loadback write has no preceding lookup of the same request and cache");
        return owner->fact.source_node_id;
    }
    // Extend is a start-only input. Its region ends at the next same-thread
    // state action; do not attach other requests' calls by temporal proximity.
    const model::HiCacheReplayFact * next = nullptr;
    for (const auto & item : facts) {
        const auto & fact = item.fact;
        if (fact.pid != pid || fact.tid != tid) continue;
        if (fact.source_ts <= begin) {
            if (!owner || fact.source_ts > owner->fact.source_ts) owner = &item;
        }
        else if (!next || fact.source_ts < next->fact.source_ts) next = &item;
    }
    if (!owner || owner->role != HiCacheFactRole::CacheExtendInput || !next || end > next->fact.source_ts)
        throw std::runtime_error("Write call lacks an enclosing allocation/lifecycle region: pid=" + pid + " at_us=" + std::to_string(begin));
    return owner->fact.source_node_id;
}

std::vector<HiCacheCapacityGuard> observe_capacity_guards(const patch::HiCacheSourceDagIndex & source, std::span<const model::HiCacheReplayFact> facts,
                                                          std::span<const core::TraceEvent> observations, uint64_t begin, uint64_t end) {
    std::vector<HiCacheCapacityGuard> guards;
    std::set<size_t> owners;
    for (const auto & event : observations) {
        if (event.name != "runtime.hicache.capacity_guard" || event.ts < begin || event.ts + event.dur > end) continue;
        if (event.arg("status") != "returned" || !event.dur) throw std::runtime_error("Capacity guard has no complete return interval");
        const auto owner = observe_write_call_owner(source, facts, event.pid, event.tid, event.ts, event.ts + event.dur);
        const auto item = std::ranges::find_if(facts, [&](const auto & fact) { return fact.fact.source_node_id == owner; });
        if (item == facts.end() || (item->role != HiCacheFactRole::CacheExtendInput && item->role != HiCacheFactRole::CacheDecodeAllocation))
            throw std::runtime_error("Capacity guard is outside an extend/decode allocation");
        if (!owners.insert(owner).second) throw std::runtime_error("Allocation has multiple capacity guards");
        guards.push_back({ owner, event.pid, event.tid, event.ts, event.ts + event.dur });
    }
    return guards;
}

void HiCacheWriteCalls::bind(core::DagGraph & graph, uint64_t begin, uint64_t end) {
    lifecycle_inserts_ = observe_lifecycle_inserts(graph, replay_.facts(), begin, end);
    for (const auto & insert : lifecycle_inserts_) (void)lifecycle_ready_steps(insert);
    const auto in_scope = [&](const std::string & pid, const std::string & tid, uint64_t first, uint64_t last) {
        if (first >= begin && last <= end) return true;
        return std::ranges::any_of(lifecycle_inserts_, [&](const auto & insert) {
            const auto & call = insert.envelope;
            return call.pid == pid && call.tid == tid && first >= call.ts && last <= call.ts + call.dur;
        });
    };
    bind_load_branches(graph, begin, end);
    bind_load_attempts(graph, begin, end);
    capacity_guards_ = observe_capacity_guards(patch::HiCacheSourceDagIndex(graph), replay_.facts(), graph.runtime_observations(), begin, end);
    std::vector<core::TraceEvent> guard_points;
    for (const auto & guard : capacity_guards_) {
        for (const auto at : { guard.begin, guard.end }) {
            core::TraceEvent point;
            point.name = "hicache.capacity_guard";
            point.pid = guard.pid;
            point.tid = guard.tid;
            point.ts = at;
            guard_points.push_back(std::move(point));
        }
    }
    const auto guard_nodes = bind_hicache_control_points(graph, guard_points);
    for (size_t i = 0; i < capacity_guards_.size(); ++i) {
        if (!guard_nodes[2 * i] || !guard_nodes[2 * i + 1]) throw std::runtime_error("Capacity guard lacks exact CPU entry/return boundaries");
        capacity_guards_[i].entry_node = *guard_nodes[2 * i];
        capacity_guards_[i].return_node = *guard_nodes[2 * i + 1];
        allocation_returns_[capacity_guards_[i].return_node].push_back(capacity_guards_[i].owner);
        guarded_owners_.insert(capacity_guards_[i].owner);
    }
    std::vector<core::TraceEvent> host_boundaries;
    const auto add_host_boundaries = [&](const core::TraceEvent & event) {
        if (!in_scope(event.pid, event.tid, event.ts, event.ts + event.dur) || !event.dur) return;
        for (const auto at : { event.ts, event.ts + event.dur }) {
            core::TraceEvent point;
            point.name = "hicache.host_region";
            point.pid = event.pid;
            point.tid = event.tid;
            point.ts = at;
            host_boundaries.push_back(std::move(point));
        }
    };
    for (const auto & event : graph.hicache_fact_events()) {
        const auto role = parse_hicache_fact_metadata(event).role;
        if ((role != "commit_device_to_host_enqueue_observed" && role != "capacity_result_observed") || event.arg("phase") != "end"
            || !in_scope(event.pid, event.tid, event.ts, event.ts + event.dur) || !event.dur)
            continue;
        add_host_boundaries(event);
    }
    for (const auto & event : graph.runtime_observations())
        if (event.name == "runtime.hicache.device_release_backup" || event.name == "runtime.hicache.device_release_regular") add_host_boundaries(event);
    // Only pure control-self remainders and gaps can be partitioned. The host
    // observer below still rejects boundaries that cut a real executable leaf.
    (void)bind_hicache_control_points(graph, host_boundaries);
    const patch::HiCacheSourceDagIndex source(graph);
    const auto ledger = patch::build_hicache_io_operation_ledger(source);
    std::vector<core::TraceEvent> points;
    std::vector<std::pair<Action, size_t>> actions;
    const auto point = [&](const std::string & pid, const std::string & tid, uint64_t at, Action action, size_t call) {
        core::TraceEvent event;
        event.name = "hicache.write_call";
        event.pid = pid;
        event.tid = tid;
        event.ts = at;
        points.push_back(std::move(event));
        actions.emplace_back(action, call);
    };
    for (const auto & record : ledger.records) {
        if (record.kind != patch::HiCacheIoOperationKind::WriteDeviceToHost || !in_scope(record.pid, record.tid, record.source_start_us, record.source_end_us))
            continue;
        const auto owner = observe_write_call_owner(source, replay_.facts(), record.pid, record.tid, record.source_start_us, record.source_end_us);
        writes_.add_source(source, record);
        point(record.pid, record.tid, record.source_start_us, Action::Submit, calls_.size());
        calls_.push_back({ .owner = owner, .source = record.timing_fact_node_id });
        source_owners_.insert(owner);
        ++submissions_;
    }
    for (const auto & event : graph.runtime_observations()) {
        if (event.name != "runtime.hicache.write_completion" || event.arg("blocking") != "true" || event.ts < begin || event.ts + event.dur > end) continue;
        if (event.arg("status") != "returned") throw std::runtime_error("Blocking write check did not return");
        const auto owner = observe_write_call_owner(source, replay_.facts(), event.pid, event.tid, event.ts, event.ts + event.dur);
        point(event.pid, event.tid, event.ts, Action::Wait, calls_.size());
        point(event.pid, event.tid, event.ts + event.dur, Action::Return, calls_.size());
        calls_.push_back({ .owner = owner, .pid = event.pid, .tid = event.tid, .at_us = event.ts });
        wait_owners_.insert(owner);
    }
    const auto nodes = bind_hicache_control_points(graph, points);
    for (size_t i = 0; i < points.size(); ++i) {
        const auto bound = nodes[i];
        if (!bound) {
            auto message = "Write call has no exact CPU boundary: pid=" + points[i].pid + " at_us=" + std::to_string(points[i].ts);
#ifdef DEBUG
            for (const auto candidate : { source.cpu_boundary_at_or_before(points[i].pid, points[i].tid, points[i].ts),
                                          source.cpu_boundary_at_or_after(points[i].pid, points[i].tid, points[i].ts) }) {
                if (!candidate) continue;
                const auto & event = graph.event_for_node(*candidate);
                message += " nearby=" + event.name + ":" + std::to_string(event.ts) + "+" + std::to_string(event.dur);
            }
#endif
            throw std::runtime_error(message);
        }
        const auto [action, call] = actions[i];
        actions_[*bound].push_back({ action, call });
        if (action == Action::Return) calls_[call].return_node = *bound;
        if (action == Action::Wait) calls_[call].entry_node = *bound;
    }
    // Call boundaries may split host self/gap pieces. Observe templates only
    // after those edits so their leaf ownership matches the execution graph.
    const patch::HiCacheSourceDagIndex bound_source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    for (const auto & record : ledger.records) {
        if (record.kind != patch::HiCacheIoOperationKind::WriteDeviceToHost || record.source_start_us < begin || record.source_end_us > end) continue;
        const auto host = observe_write_host_template(bound_source, record);
        try {
            auto expansion = prepare_write_expansion(bound_source, queues, record, host, writes_.source_completion(record.timing_fact_node_id));
            auto lanes = expansion.resource_lanes(graph);
            templates_.emplace(record.timing_fact_node_id,
                               WriteTemplate{ host.write_back,
                                              writes_.source_start_record(bound_source, record.timing_fact_node_id),
                                              std::move(expansion),
                                              record.pid,
                                              record.tid,
                                              std::move(lanes) });
        }
        catch (const std::exception & error) {
            throw std::runtime_error("Write template pid=" + record.pid + " at_us=" + std::to_string(record.source_start_us) + ": " + error.what());
        }
    }
    for (auto & call : calls_) {
        if (call.source) continue;
        for (const auto edge_id : bound_source.outgoing_edge_ids(call.entry_node)) {
            const auto & edge = graph.edge(edge_id);
            if (edge.active && graph.node(edge.dst).active) call.successors.push_back(edge.dst);
        }
        for (const auto & [id, donor] : templates_) {
            if (donor.pid != call.pid || donor.tid != call.tid) continue;
            const auto locate = [&](size_t sample) {
                const auto lane = graph.node(sample).lane_id;
                if (!call.positions.contains(lane))
                    call.positions.emplace(lane, observe_write_stream_position(bound_source, sample, call.pid, call.tid, call.at_us));
            };
            for (const auto node : donor.expansion.resource_nodes()) locate(node);
        }
    }
    // Prepare the work outside complete write calls. These source regions are
    // donors, not target victim counts; actual insertion is driven by target
    // allocation state. Keep existing source work unchanged during preparation.
    for (const auto & eviction : bound_source.fact_nodes()) {
        if (eviction.fact_role != "capacity_result_observed" || eviction.phase != "end" || eviction.timestamp_us < begin
            || eviction.timestamp_us + eviction.duration_us > end)
            continue;
        for (const auto & region : observe_eviction_regions(bound_source, eviction)) {
            if (region.kind == HiCacheEvictionRegion::Kind::WriteBackup) continue;
            try {
                const auto host = observe_host_template(bound_source, eviction.pid, eviction.tid, region.begin, region.end);
                eviction_controls_.push_back({ eviction.pid, eviction.tid, region, prepare_host_expansion(bound_source, queues, host) });
            }
            catch (const std::exception & error) {
                throw std::runtime_error("Eviction control pid=" + eviction.pid + " at_us=" + std::to_string(region.begin)
                                         + " end_us=" + std::to_string(region.end) + ": " + error.what());
            }
        }
    }
    load_write_calibration(graph, queues);
    load_release_calibration(graph, queues);
    prepare_load_capacity(bound_source, queues);
    prepare_load_admissions(bound_source, queues);
    prepare_generated_load_admissions(bound_source, queues);
    replace_source_evictions(graph, begin, end);
    bind_capacity_calls(patch::HiCacheSourceDagIndex(graph));
    prepare_empty_load_branches(graph);
    bind_lifecycle_writes(graph);
}

void HiCacheWriteCalls::applied(const model::HiCacheReplayFact & item) const {
    const auto & fact = item.fact;
    const auto location = [&] {
        return ": role=" + fact.role + " pid=" + fact.pid + " source_us=" + std::to_string(fact.source_ts) + " fact=" + std::to_string(fact.source_node_id);
    };
    if (replay_.state().allocation_pending(fact) && !wait_owners_.contains(fact.source_node_id) && !capacity_calls_.contains(fact.source_node_id)) {
        auto detail = location() + " capacity_guard=" + (guarded_owners_.contains(fact.source_node_id) ? "observed" : "missing");
        if (const auto * work = replay_.state().eviction_work(fact)) {
            detail += " requested_pages=" + std::to_string(work->requested_pages) + " victims=[";
            for (const auto & victim : work->victims)
                detail += "node=" + std::to_string(victim.node) + ":pages=" + std::to_string(victim.page_count)
                          + (victim.release_after_write ? ":after_write" : ":during_selection") + (victim.backed_up ? ":backup;" : ":regular;");
            detail += "]";
        }
        detail += " pending_writes=[";
        for (const auto & write : replay_.state().pending_device_writes(fact))
            detail += "node=" + std::to_string(write.node) + ":bytes=" + std::to_string(write.schedule.effective_byte_count)
                      + ":owner=" + std::to_string(write.header.source_node_id) + ";";
        throw std::runtime_error("Target capacity branch is not assembled" + detail + "]");
    }
    for (const auto & write : replay_.state().pending_device_writes(fact))
        if (write.header.source_node_id == fact.source_node_id && !source_owners_.contains(fact.source_node_id)
            && !lifecycle_write_owners_.contains(fact.source_node_id) && !capacity_calls_.contains(fact.source_node_id)
            && !(wait_owners_.contains(fact.source_node_id)
                 && std::ranges::any_of(templates_, [&](const auto & entry) { return entry.second.pid == fact.pid && entry.second.tid == fact.tid; })))
            throw std::runtime_error("Target adds a write without a host submission template" + location());
}

std::vector<size_t> HiCacheWriteCalls::advance(size_t node, uint64_t time, simulation::FutureDag & future) {
    writes_.advance(node, time);
    submit_lifecycle_writes(node, time, future);
    if (empty_load_branches_.contains(node)) start_load_branch(node, time, future);
    std::vector<size_t> resumed;
    const auto at_capacity = [&](size_t owner) {
        const auto & item = replay_.fact(owner);
        if (!item.started) throw std::runtime_error("Capacity call precedes its allocation input");
        auto fact = item.fact;
        fact.ts = time;
        fact.execution_anchor_node_id = node;
        return fact;
    };
    if (const auto ack = capacity_ack_.find(node); ack != capacity_ack_.end()) {
        const auto fact = at_capacity(ack->second);
        if (replay_.state().allocation_pending(fact)) replay_.state().confirm_allocation_writes(fact);
    }
    if (const auto capacity = capacity_at_.find(node); capacity != capacity_at_.end())
        expand_capacity(capacity_calls_.at(capacity->second), at_capacity(capacity->second), future);
    const auto found = actions_.find(node);
    if (found != actions_.end())
        for (const auto [action, id] : found->second) {
            auto & call = calls_[id];
            const auto & item = replay_.fact(call.owner);
            if (!item.started) throw std::runtime_error("Write host call precedes its state action; bind the action's entry before issuing its writes");
            auto fact = item.fact;
            fact.ts = time;
            fact.execution_anchor_node_id = node;
            if (action == Action::Submit) {
                std::optional<model::HiCacheDeviceWrite> operation;
                for (const auto & pending : replay_.state().pending_device_writes(fact))
                    if (pending.header.source_node_id == call.owner && !completions_.contains({ pending.header.cache_scope, pending.header.operation_id })) {
                        operation = pending;
                        break;
                    }
                const auto completion = writes_.submit_source(*call.source, fact, operation, future);
                if (completion) completions_.emplace(std::pair{ operation->header.cache_scope, operation->header.operation_id }, *completion);
            }
            else if (action == Action::Wait) {
                call.waiting = replay_.state().allocation_pending(fact);
                HiCacheHostSequence sequence(call.positions, stream_insertions_, future);
                for (const auto & pending : replay_.state().pending_device_writes(fact)) {
                    const auto key = std::pair{ pending.header.cache_scope, pending.header.operation_id };
                    auto complete = completions_.find(key);
                    if (complete == completions_.end()) {
                        if (pending.header.source_node_id != call.owner) throw std::runtime_error("Unsubmitted target write belongs to another host action");
                        const auto * donor = &select_write_template(call.pid, call.tid, pending.schedule.effective_byte_count);
                        const auto projected = resize_write_pages(donor->expansion, pending.schedule.effective_byte_count, page_bytes_);
                        const auto expanded = sequence.append(projected, pending.schedule.duration_us, donor->position_lanes);
                        for (const auto successor : call.successors) future.depend(expanded.host_return, successor);
                        const auto done = writes_.submit_expanded(fact, pending, expanded.write_start, expanded.completion, future);
                        complete = completions_.emplace(key, done).first;
                        ++expanded_;
                    }
                    if (call.waiting) future.depend(complete->second, call.return_node);
                }
            }
            else if (call.waiting) {
                if (guarded_owners_.contains(call.owner)) replay_.state().confirm_allocation_writes(fact);
                else {
                    replay_.resume(call.owner, time, node);
                    resumed.push_back(call.owner);
                    ++resumed_;
                }
            }
        }
    // The source capacity return follows CPU confirmation and the subsequent
    // device-page release calls. Publishing allocation at the earlier write
    // check would let target preparation start before this work has finished.
    if (const auto returns = allocation_returns_.find(node); returns != allocation_returns_.end())
        for (const auto owner : returns->second) {
            const auto & item = replay_.fact(owner);
            if (!replay_.state().allocation_pending(item.fact)) continue;
            replay_.resume(owner, time, node);
            resumed.push_back(owner);
            ++resumed_;
        }
    if (const auto at = load_branch_returns_.find(node); at != load_branch_returns_.end()) {
        const auto & branch = at->second;
        const auto * work = replay_.state().load_admission_work(replay_.fact(branch.owner).fact);
        if (!work) throw std::logic_error("Load allocation return lost its target work");
        if (branch.expected_allocation && work->allocated != *branch.expected_allocation)
            throw std::logic_error("Load allocation return disagrees with the projected target capacity outcome");
        if (!work->allocated && branch.donor) throw std::runtime_error("New load admission needs a failed-return template after capacity exhaustion");
        HiCacheHostSequence suffix(capacity_calls_.at(branch.owner).positions, stream_insertions_, future);
        size_t returned;
        if (branch.donor) {
            const auto & donor = load_admissions_.at(*branch.donor);
            returned = suffix.append(donor.suffix, donor.suffix_lanes).host_return;
        }
        else {
            const auto & fact = replay_.fact(branch.owner).fact;
            const auto & program = generated_load_admissions_.at({ fact.pid, fact.tid });
            for (const auto pages : work->allocated ? work->node_pages : std::vector<uint64_t>{}) {
                if (!pages) throw std::logic_error("Target load contains an empty promoted node");
                (void)suffix.append(program.clone, program.lanes);
            }
            if (work->allocated) returned = suffix.append(program.tail, {}).host_return;
            else {
                auto cleanup = load_failure_templates_.at(branch.owner);
                for (auto & part : cleanup.nodes) part.work.name = "target failed load cleanup: base control cost proxy";
                returned = suffix.append(cleanup, {}).host_return;
            }
        }
        future.depend(returned, branch.continuation);
    }
    return resumed;
}

} // namespace markov::trace_graph::modules::hicache::runtime
