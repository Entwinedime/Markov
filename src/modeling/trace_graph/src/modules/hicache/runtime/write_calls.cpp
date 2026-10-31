#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/host_removal.hpp"
#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheWriteCalls::bind_call_resources(const adapters::sglang::HiCacheSourceIndex & source, Call & call, std::span<const size_t> resources,
                                            bool candidate) {
    for (const auto node : resources) {
        const auto lane = source.graph().node(node).lane_id;
        if (call.positions.contains(lane)) continue;

        if (!candidate) {
            call.positions.emplace(lane, observe_write_stream_position(source, node, call.pid, call.tid, call.at_us));
            continue;
        }

        // Missing candidate resources matter only if the target selects them.
        // Keep the original observation failure for that later demand.
        auto & issues = capacity_position_errors_[call.owner];
        if (issues.contains(lane)) continue;

        try {
            call.positions.emplace(lane, observe_write_stream_position(source, node, call.pid, call.tid, call.at_us));
        }
        catch (const std::runtime_error & error) {
            issues.emplace(lane, error.what());
        }
    }
}

void HiCacheWriteCalls::prepare_write_resources(core::DagGraph & graph, const simulation::detail::CpuTaskQueues & queues) {
    const adapters::sglang::HiCacheSourceIndex source(graph);
    const auto bind = [&](const std::string & pid, const std::string & tid, size_t main, size_t compute, std::optional<size_t> transfer) {
        const auto key = std::pair{ pid, tid };
        if (write_resources_.contains(key)) return;
        const auto resources = observe_host_device_resources(source, queues, main, compute);
        if (!resources) return;

        // A legal target write can introduce its controller transfer stream
        // even when this source only freed device pages. Cost evidence does
        // not decide whether the resource or execution branch exists.
        const auto payload = transfer ? *transfer : graph.add_device_resource("hicache.write:" + pid + ":" + tid, graph.node(main).gpu_id);
        const auto compute_lane = graph.node(compute).lane_id;
        const auto payload_lane = graph.node(payload).lane_id;
        write_resources_.emplace(key,
                                 WriteResources{
                                     compute,
                                     resources->worker,
                                     payload,
                                     { compute_lane, payload_lane, compute_lane }
        });
    };
    for (const auto & [id, sample] : observed_write_fragments_) { bind(sample.pid, sample.tid, sample.main, sample.compute_record, sample.payload); }
    for (const auto & sample : observed_eviction_fragments_) {
        if (!sample.main) continue;

        bind(sample.pid, sample.tid, *sample.main, *sample.compute, std::nullopt);
    }
    // A lifecycle can gain a write under the target policy even when the
    // source never submitted one. Its scheduler stream/worker role is shared
    // with allocation; no source write template or cost is needed to bind it.
    for (const auto & insert : lifecycle_inserts_) {
        const auto & event = insert.envelope;
        const auto compute = compute_resources_.find({ event.pid, event.tid });
        if (compute == compute_resources_.end()) continue;
        if (const auto main = source.cpu_boundary_at_or_before(event.pid, event.tid, event.ts))
            bind(event.pid, event.tid, *main, compute->second, std::nullopt);
    }

    for (auto & call : calls_) {
        if (call.source) continue;
        const auto compute = compute_resources_.find({ call.pid, call.tid });
        if (compute != compute_resources_.end()) {
            if (const auto main = source.cpu_boundary_at_or_before(call.pid, call.tid, call.at_us))
                bind(call.pid, call.tid, *main, compute->second, std::nullopt);
        }

        const auto resources = write_resources_.find({ call.pid, call.tid });
        if (resources == write_resources_.end()) continue;
        for (const auto node : { resources->second.compute, resources->second.transfer }) {
            const auto lane = graph.node(node).lane_id;
            if (!call.positions.contains(lane)) call.positions.emplace(lane, observe_write_stream_position(source, node, call.pid, call.tid, call.at_us));
        }
    }
}

std::vector<HiCacheHostExpansion *> HiCacheWriteCalls::host_plans() {
    std::vector<HiCacheHostExpansion *> plans;
    // Observed write/eviction fragments now serve only removal and resource
    // identification. Only programs that will actually execute need worker
    // rebinding; an obsolete source worker is not a target cost requirement.

    for (auto & donor : load_admissions_) {
        if (donor.clone) plans.push_back(&donor.clone->program.plan);
        for (auto & step : donor.allocation) plans.push_back(&step.program.plan);
        plans.push_back(&donor.prefix_concat.program.plan);
        if (allocator_need_sort_) {
            plans.push_back(&donor.merge_concat.program.plan);
            plans.push_back(&donor.merge_sort.program.plan);
        }
    }

    for (auto & [id, plan] : load_failure_templates_) plans.push_back(&plan);
    return plans;
}

HiCacheExpandedWrite HiCacheWriteCalls::submit_target_write(const WriteResources & resources, const HiCacheFact & fact,
                                                            const model::HiCacheDeviceWrite & operation, HiCacheHostSequence & sequence,
                                                            simulation::FutureDag & future) {
    const auto bytes = operation.schedule.effective_byte_count;
    if (!page_bytes_ || !bytes || bytes % page_bytes_) throw std::runtime_error("Target write requires a positive whole-page payload");

    auto generated = generate_page_first_submission(*graph_,
                                                    fact.execution_anchor_node_id.value(),
                                                    resources.worker,
                                                    resources.compute,
                                                    resources.transfer,
                                                    bytes / page_bytes_,
                                                    page_bytes_,
                                                    1,
                                                    "write",
                                                    write_back_,
                                                    compact_programs_);
    bind_host_operations(generated.plan, generated.operations, operator_provider_);
    const auto lanes = generated.plan.resource_lanes(*graph_);
    const auto service_us = io_completion_.duration("write_device_to_host", page_bytes_, bytes / page_bytes_, 0);
    bind_transfer_payload(generated.plan, service_us);
    auto expanded = sequence.append(std::move(generated.plan), lanes);
    const auto completion = writes_.submit_expanded(fact, operation, expanded.write_start, expanded.completion, future);

    completions_.emplace(std::pair{ operation.header.cache_scope, operation.header.operation_id }, completion);
    ++expanded_;
    return expanded;
}

size_t observe_write_call_owner(const adapters::sglang::HiCacheSourceIndex & source, std::span<const model::HiCacheReplayFact> facts, const std::string & pid,
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
    const adapters::sglang::HiCacheFactObservation * admission = nullptr;
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

std::vector<HiCacheCapacityGuard> observe_capacity_guards(const adapters::sglang::HiCacheSourceIndex & source, std::span<const model::HiCacheReplayFact> facts,
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

void HiCacheWriteCalls::bind(core::DagGraph & graph, uint64_t begin, uint64_t end, size_t opening_node) {
    graph_ = &graph;
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
    capacity_guards_ = observe_capacity_guards(adapters::sglang::HiCacheSourceIndex(graph), replay_.facts(), graph.runtime_observations(), begin, end);
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
        for (const auto at : { std::max(event.ts, begin), event.ts + event.dur }) {
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
    const adapters::sglang::HiCacheSourceIndex source(graph);
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
        const auto & envelope = observe_write_envelope(source, record.pid, record.tid, record.source_start_us, record.source_end_us);
        writes_.add_source(source, record);
        point(record.pid, record.tid, std::max(envelope.timestamp_us, begin), Action::Submit, calls_.size());
        calls_.push_back({ .owner = owner, .source = record.timing_fact_node_id, .pid = record.pid, .tid = record.tid, .at_us = envelope.timestamp_us });
        point(record.pid, record.tid, envelope.timestamp_us + envelope.duration_us, Action::Return, calls_.size() - 1);
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
    core::DagMutationPlan opening{ .component = "hicache_write_opening" };
    for (size_t i = 0; i < points.size(); ++i) {
        // Keep partial-call entry on its real CPU lane. The initial lifecycle
        // state is applied first, without reconstructing pre-window service.
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
        if (action == Action::Wait || action == Action::Submit) calls_[call].entry_node = *bound;
        if (action == Action::Submit && calls_[call].at_us < begin)
            opening.add_edges.push_back({ core::DagNodeRef::existing(opening_node), core::DagNodeRef::existing(*bound), core::DagEdgeKind::Mutation });
    }
    if (!opening.add_edges.empty()) (void)core::apply_dag_mutation_plan(graph, opening);
    // Call boundaries may split host self/gap pieces. Observe fragments only
    // after those edits so their leaf ownership matches the execution graph.
    const adapters::sglang::HiCacheSourceIndex bound_source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    for (const auto & record : ledger.records) {
        if (record.kind != patch::HiCacheIoOperationKind::WriteDeviceToHost || record.source_start_us < begin || record.source_end_us > end) continue;
        const auto host = observe_write_host_template(bound_source, record);
        try {
            auto expansion = prepare_write_expansion(bound_source, queues, record, host, writes_.source_completion(record.timing_fact_node_id));
            const auto compute_record = writes_.source_start_record(bound_source, record.timing_fact_node_id);
            const auto roles = write_resource_roles(graph, expansion, compute_record);

            observed_write_fragments_.emplace(
                record.timing_fact_node_id,
                ObservedWriteFragment{ record.pid, record.tid, roles.at("main"), compute_record, roles.at("payload"), expansion.resource_nodes() });
        }
        catch (const std::exception & error) {
            throw std::runtime_error("Observed write pid=" + record.pid + " at_us=" + std::to_string(record.source_start_us) + ": " + error.what());
        }
    }
    for (auto & call : calls_) {
        if (call.source) continue;
        for (const auto edge_id : bound_source.outgoing_edge_ids(call.entry_node)) {
            const auto & edge = graph.edge(edge_id);
            if (edge.active && graph.node(edge.dst).active) call.successors.push_back(edge.dst);
        }
        for (const auto & [id, donor] : observed_write_fragments_) {
            if (donor.pid != call.pid || donor.tid != call.tid) continue;
            bind_call_resources(bound_source, call, donor.resources);
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
                const auto expansion = prepare_host_expansion(bound_source, queues, host);
                std::optional<size_t> main, compute;
                using Kind = HiCacheEvictionRegion::Kind;
                if ((region.kind == Kind::ReleaseRegular || region.kind == Kind::ReleaseBackup) && expansion.streams.size() == 1) {
                    compute = expansion.streams.front().source_node;
                    const auto & main_lane = expansion.nodes.at(expansion.host_return).work.lane_key;
                    for (const auto & [original, local] : expansion.source_nodes) {
                        if (!graph.node(original).is_cpu || graph.node_lane_key(original) != main_lane) continue;

                        main = original;
                        break;
                    }
                }

                observed_eviction_fragments_.push_back({ eviction.pid, eviction.tid, expansion.resource_nodes(), main, compute });
            }
            catch (const std::exception & error) {
                throw std::runtime_error("Eviction control pid=" + eviction.pid + " at_us=" + std::to_string(region.begin)
                                         + " end_us=" + std::to_string(region.end) + ": " + error.what());
            }
        }
    }
    prepare_write_resources(graph, queues);
    prepare_generated_load_admissions(bound_source, queues);
    replace_source_evictions(graph, begin, end);
    replace_source_write_calls(graph);
    bind_capacity_calls(adapters::sglang::HiCacheSourceIndex(graph));
    prepare_empty_load_branches(graph);
    bind_lifecycle_writes(graph, begin);
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
            && !(wait_owners_.contains(fact.source_node_id) && write_resources_.contains({ fact.pid, fact.tid })))
            throw std::runtime_error("Target adds a write without a host execution site or resource binding" + location());
}

std::vector<size_t> HiCacheWriteCalls::advance(size_t node, uint64_t time, simulation::FutureDag & future) {
    writes_.advance(node, time);
    submit_lifecycle_writes(node, time, future);
    std::vector<size_t> resumed;
    if (const auto branch = empty_load_branches_.find(node); branch != empty_load_branches_.end()) {
        const auto owner = branch->second.owner;
        replay_.admit_load(owner, time, node);
        applied(replay_.fact(owner));
        start_load_branch(node, time, future);
        if (replay_.fact(owner).consumed) resumed.push_back(owner);
    }

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
        // Each generated return follows one event wait and its acknowledgement
        // body. Publish host residency and enqueue H2S at this FIFO boundary;
        // allocation release still waits for the whole blocking check.
        replay_.state().acknowledge_writes(fact, 1);
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
                HiCacheHostSequence sequence(call.positions, stream_insertions_, future);
                for (const auto & pending : replay_.state().pending_device_writes(fact)) {
                    const auto key = std::pair{ pending.header.cache_scope, pending.header.operation_id };
                    if (pending.header.source_node_id != call.owner || completions_.contains(key)) continue;

                    const auto & resources = write_resources_.at({ call.pid, call.tid });
                    const auto expanded = submit_target_write(resources, fact, pending, sequence, future);
                    for (const auto successor : call.successors) future.depend(expanded.host_return, successor);
                }
            }
            else if (action == Action::Wait) {
                call.waiting = replay_.state().allocation_pending(fact);
                HiCacheHostSequence sequence(call.positions, stream_insertions_, future);
                for (const auto & pending : replay_.state().pending_device_writes(fact)) {
                    const auto key = std::pair{ pending.header.cache_scope, pending.header.operation_id };
                    auto complete = completions_.find(key);
                    if (complete == completions_.end()) {
                        if (pending.header.source_node_id != call.owner) throw std::runtime_error("Unsubmitted target write belongs to another host action");
                        const auto & donor = write_resources_.at({ call.pid, call.tid });
                        const auto expanded = submit_target_write(donor, fact, pending, sequence, future);
                        for (const auto successor : call.successors) future.depend(expanded.host_return, successor);
                        complete = completions_.find(key);
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
        if (work->allocated != branch.expected_allocation)
            throw std::logic_error("Load allocation return disagrees with the projected target capacity outcome");
        const auto & program = load_admissions_.at(branch.program);
        HiCacheHostSequence suffix(capacity_calls_.at(branch.owner).positions, stream_insertions_, future, node);
        size_t returned;
        if (work->allocated) {
            if (program.clone) {
                bool first_clone = true;
                for (const auto pages : work->node_pages) {
                    if (!pages) throw std::logic_error("Target load contains an empty promoted node");
                    (void)suffix.append(cost_load_step(*program.clone, pages, first_clone), program.clone->lanes);
                    first_clone = false;
                }
            }
            returned = suffix.append(load_admission_tail(program, work->requested_pages, true), {}).host_return;
        }
        else {
            // Allocation failure and the caller's bookkeeping are different
            // operations. Never charge the failed allocator a second time
            // as a proxy for the post-return cleanup.
            returned = suffix.append(load_admission_tail(program, work->requested_pages, false), {}).host_return;
        }
        auto prefix = load_prefix_concat(program, *work);
        const bool device_prefix = work->allocated || work->existing_prefix_pages > 0;
        returned =
            suffix.append(std::move(prefix), device_prefix ? std::span<const size_t>{ program.prefix_concat.lanes } : std::span<const size_t>{}).host_return;
        future.depend(returned, branch.continuation);
    }
    return resumed;
}

} // namespace markov::trace_graph::modules::hicache::runtime
