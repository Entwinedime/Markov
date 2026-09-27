#include "markov/trace_graph/modules/hicache/runtime/queue_confirmations.hpp"
#include "markov/trace_graph/core/cpu_gap_observation.hpp"
#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/modules/hicache/prefetch_worker_observation.hpp"
#include "markov/trace_graph/modules/hicache/runtime/host_removal.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheQueueConfirmations::bind(core::DagGraph & graph, model::HiCacheModelReplay & replay, uint64_t begin, uint64_t finish,
                                     const std::vector<core::TraceEvent> & replaced, ReturnObserver observer, WriteWork write_work,
                                     std::optional<uint64_t> idle_since_us, bool generate_load_tails) {
    if (state_) throw std::logic_error("Queue confirmations are already bound");
    state_ = &replay.state();
    observer_ = std::move(observer);
    write_work_ = std::move(write_work);
    const patch::HiCacheSourceDagIndex source(graph);
    const auto collectives = observe_cpu_collectives(source);
    const auto workers = observe_prefetch_workers(source, collectives);
    const auto & events = graph.runtime_observations();
    std::vector<core::TraceEvent> points;
    std::vector<Point> actions;
    for (size_t round_id = 0; round_id < collectives.rounds.size(); ++round_id) {
        const auto & round = collectives.rounds[round_id];
        if (round.calls.empty()) continue;
        const auto role = events.at(round.calls.front().observation).arg("role");
        const bool storage = role == "storage_control_drain";
        const bool write = role == "write_completion_check";
        if (!storage && !write && role != "load_completion_check") continue;
        const auto removed = [&](const auto & call) {
            const auto & event = events.at(call.observation);
            return std::ranges::any_of(replaced, [&](const auto & span) {
                return span.pid == event.pid && span.tid == event.tid && span.ts <= event.ts && span.ts + span.dur >= event.ts + event.dur;
            });
        };
        if (std::ranges::any_of(round.calls, removed)) {
            if (!std::ranges::all_of(round.calls, removed)) throw std::runtime_error("Only part of a confirmation round was replaced");
            ++coverage_.replaced_rounds;
            continue;
        }
        const auto inside = [&](const auto & call) {
            const auto & event = events.at(call.observation);
            return event.ts >= begin && event.ts + event.dur <= finish;
        };
        if (!std::ranges::any_of(round.calls, inside)) continue;
        if (!std::ranges::all_of(round.calls, inside) || round.calls.size() != round.members.size()) {
            ++coverage_.partial_window_rounds;
            continue;
        }
        if (!round.issue.empty()) throw std::runtime_error("Queue confirmation round: " + round.issue);
        Check check;
        check.storage = storage;
        check.write = write;
        std::vector<const core::TraceEvent *> envelopes;
        for (const auto & call : round.calls) {
            const auto & event = events.at(call.observation);
            const core::TraceEvent * envelope = nullptr;
            if (storage) {
                for (const auto & drain : workers.drains) {
                    const auto & candidate = events[drain.observation];
                    if (candidate.pid != event.pid || std::ranges::find(drain.agreement_rounds, round_id) == drain.agreement_rounds.end()) continue;
                    if (envelope || !drain.issue.empty() || drain.agreement_rounds.size() != 1)
                        throw std::runtime_error("Storage drain needs a unique complete single-group observation");
                    envelope = &candidate;
                }
            }
            else {
                const auto name = write ? "runtime.hicache.write_completion" : "runtime.hicache.load_completion";
                for (const auto & candidate : events)
                    if (candidate.name == name && candidate.pid == event.pid && candidate.tid == event.tid
                        && candidate.ts <= event.ts && candidate.ts + candidate.dur >= event.ts + event.dur) {
                        if (envelope) throw std::runtime_error("Multiple completion check envelopes");
                        envelope = &candidate;
                    }
            }
            if (!envelope || envelope->arg("status") != "returned" || (!storage && envelope->arg("blocking") != "false"))
                throw std::runtime_error("Missing successful nonblocking load/write or storage confirmation envelope");
            envelopes.push_back(envelope);
        }
        // A complete in-window MIN may apply state after the HTTP endpoint.
        // Bind that retained return below; it is execution context, not a new
        // scoring endpoint. A pre-window prefix needs the workload's explicit
        // idle barrier, not a source queue count or an arbitrary time margin.
        if (std::ranges::any_of(envelopes, [&](const auto * event) {
                return event->ts < begin && (!idle_since_us || event->ts < *idle_since_us);
            })) {
            ++coverage_.partial_window_rounds;
            continue;
        }
        for (size_t i = 0; i < round.calls.size(); ++i) {
            const auto & call = round.calls[i];
            const auto & event = events.at(call.observation);
            const auto * envelope = envelopes[i];
            if (envelope->ts < begin) check.idle_prefix_ranks.insert(call.rank);
            if (!call.issue.empty()) throw std::runtime_error("Queue confirmation call: " + call.issue
                + " pid=" + event.pid + " role=" + role + " start_us=" + std::to_string(event.ts)
                + " duration_us=" + std::to_string(event.dur));
            std::optional<HiCacheFact> identity;
            for (const auto & item : replay.facts())
                if (item.fact.pid == event.pid) {
                    if (identity && identity->cache_scope != item.fact.cache_scope) throw std::runtime_error("Ambiguous cache scope for queue confirmation");
                    identity = item.fact;
                }
            if (!identity) throw std::runtime_error("Queue confirmation has no observed cache identity");
            check.facts[call.rank] = *identity;
            for (const auto phase : { Phase::Sample, Phase::Work, Phase::Apply }) {
                if (phase == Phase::Work && (storage || (write ? !write_work_ : !generate_load_tails))) continue;
                core::TraceEvent point;
                point.name = phase == Phase::Apply ? "queue_confirmation_return"
                             : phase == Phase::Work ? "queue_confirmation_work" : "queue_confirmation_sample";
                point.pid = event.pid;
                point.tid = event.tid;
                point.ts = phase == Phase::Apply ? envelope->ts + envelope->dur
                           : phase == Phase::Work ? event.ts + event.dur : event.ts;
                points.push_back(std::move(point));
                actions.push_back({ checks_.size(), call.rank, phase });
            }
        }
        checks_.push_back(std::move(check));
        ++(storage ? coverage_.storage_rounds : write ? coverage_.write_rounds : coverage_.load_rounds);
    }
    const auto nodes = core::insert_cpu_gap_points(graph, points);
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto bound = nodes[i];
        if (!bound) bound = source.cpu_node_starting_at(points[i].pid, points[i].tid, points[i].ts);
        if (!bound)
            throw std::runtime_error("Queue confirmation boundary unresolved: " + points[i].name + " pid=" + points[i].pid + " tid=" + points[i].tid
                                     + " at_us=" + std::to_string(points[i].ts));
        at_[*bound].push_back(actions[i]);
        if (actions[i].phase == Phase::Apply) checks_[actions[i].check].return_nodes[actions[i].rank] = *bound;
        else if (actions[i].phase == Phase::Sample) checks_[actions[i].check].sample_nodes[actions[i].rank] = *bound;
        else checks_[actions[i].check].work_nodes[actions[i].rank] = *bound;
    }
    for (const auto & [node, actions] : at_)
        for (const auto & action : actions)
            if (action.phase == Phase::Work && checks_[action.check].return_nodes.at(action.rank) == node)
                throw std::runtime_error("Write confirmation needs distinct MIN and state-return boundaries");
    // A MIN result cannot be used until every member has supplied its sample.
    // Keep native communication dependencies too; this adds no service cost.
    core::DagMutationPlan agreement{.component = "queue_confirmation_agreement"};
    for (const auto & check : checks_)
        for (const auto & [rank, sample] : check.sample_nodes)
            for (const auto & [member, use] : check.work_nodes.empty() ? check.return_nodes : check.work_nodes)
                if (sample != use) agreement.add_edges.push_back({core::DagNodeRef::existing(sample), core::DagNodeRef::existing(use), core::DagEdgeKind::Mutation});
    (void)core::apply_dag_mutation_plan(graph, agreement);
}

void HiCacheQueueConfirmations::replace_load_tails(core::DagGraph & graph) {
    const patch::HiCacheSourceDagIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    struct Tail { size_t entry, exit; HiCacheHostTemplate host; };
    struct Samples { double empty_cpu = 0, empty_gap = 0, cpu = 0, gap = 0; size_t empty = 0, batches = 0; };
    std::map<std::pair<std::string, std::string>, Samples> samples;
    std::vector<Tail> tails;
    for (const auto & check : checks_) {
        if (check.storage || check.write) continue;
        for (const auto & [rank, entry] : check.work_nodes) {
            const auto exit = check.return_nodes.at(rank);
            const auto & first = graph.event_for_node(entry);
            const auto & last = graph.event_for_node(exit);
            auto host = observe_host_template(source, first.pid, first.tid, first.ts, last.ts);
            const auto plan = prepare_host_expansion(source, queues, host);
            if (!host.worker_nodes.empty() || !plan.streams.empty() || !plan.waits.empty())
                throw std::runtime_error("Load confirmation tail needs CPU bookkeeping and completed-event checks only");
            double cpu = 0, gap = 0;
            for (const auto & part : plan.nodes) {
                if (!part.work.is_cpu) throw std::runtime_error("Load confirmation tail contains device work");
                cpu += part.work.duration;
                gap += part.work.cpu_gap_after;
            }
            auto & observed = samples[{first.pid, first.tid}];
            if (plan.event_waits.empty()) {
                observed.empty_cpu += cpu; observed.empty_gap += gap; ++observed.empty;
            } else {
                observed.cpu += cpu; observed.gap += gap; observed.batches += plan.event_waits.size();
            }
            load_tail_lanes_.emplace(entry, std::string(graph.node_lane_key(entry)));
            tails.push_back({entry, exit, std::move(host)});
        }
    }
    for (const auto & [lane, observed] : samples) {
        if (!observed.empty) throw std::runtime_error("Base load confirmations lack an empty CPU-tail cost sample");
        LoadTailCost cost;
        cost.empty_cpu = observed.empty_cpu / observed.empty;
        cost.empty_gap = observed.empty_gap / observed.empty;
        cost.batch_proxy = !observed.batches;
        cost.batch_cpu = observed.batches ? observed.cpu / observed.batches : cost.empty_cpu;
        cost.batch_gap = observed.batches ? observed.gap / observed.batches : cost.empty_gap;
        load_tail_costs_.emplace(lane, cost);
    }
    std::vector<HiCacheHostRegion> regions;
    for (const auto & tail : tails) regions.push_back({tail.entry, tail.exit, &tail.host});
    // These are complete, bound MIN rounds. Target completion is sampled by
    // the queue state, so old source EventSynchronize dependencies must leave
    // with the old tail. Reference release remains at the generated return.
    (void)core::apply_dag_mutation_plan(graph, plan_host_removal(source, regions));
}

void HiCacheQueueConfirmations::advance(size_t node, uint64_t time, simulation::FutureDag & future) {
    const auto found = at_.find(node);
    if (found == at_.end()) return;
    for (const auto & action : found->second) {
        auto & check = checks_.at(action.check);
        auto fact = check.facts.at(action.rank);
        fact.ts = time;
        fact.execution_anchor_node_id = node;
        if (action.phase == Phase::Sample) {
            auto & sample = check.samples[action.rank];
            if (check.storage) sample.storage = state_->prefetch_queue_sizes(fact);
            else if (check.write) sample.writes = state_->write_completion_count(fact);
            else sample.loads = state_->load_completion_count(fact);
            if (check.idle_prefix_ranks.contains(action.rank)
                && (sample.loads || sample.writes || sample.storage.revoked_operations
                    || sample.storage.backup_acks || sample.storage.released_pages))
                throw std::runtime_error("Pre-window confirmation no longer has an idle target prefix");
            continue;
        }
        if (check.samples.size() != check.facts.size()) throw std::runtime_error("Queue MIN returned before all rank samples");
        auto common = check.samples.begin()->second;
        for (const auto & [rank, sample] : check.samples) {
            common.loads = std::min(common.loads, sample.loads);
            common.writes = std::min(common.writes, sample.writes);
            common.storage.revoked_operations = std::min(common.storage.revoked_operations, sample.storage.revoked_operations);
            common.storage.backup_acks = std::min(common.storage.backup_acks, sample.storage.backup_acks);
            common.storage.released_pages = std::min(common.storage.released_pages, sample.storage.released_pages);
        }
        if (action.phase == Phase::Work) {
            if (check.write) {
                if (const auto last = write_work_({action.check, action.rank, fact, common.writes}, future))
                    future.depend(*last, check.return_nodes.at(action.rank));
            } else {
                if (common.loads > state_->load_completion_count(fact))
                    throw std::logic_error("Target load confirmation prefix is no longer complete");
                auto & cost = load_tail_costs_.at({fact.pid, fact.tid});
                std::optional<size_t> previous = node;
                for (uint64_t i = 0; i < std::max(uint64_t{1}, common.loads); ++i) {
                    const auto plan = generated_cpu_control(load_tail_lanes_.at(node),
                        common.loads ? cost.batch_cpu : cost.empty_cpu,
                        common.loads ? cost.batch_gap : cost.empty_gap, cost.remainder,
                        common.loads ? (cost.batch_proxy ? "target load acknowledgement: base empty-tail cost proxy"
                                                       : "target load acknowledgement: base per-batch CPU cost")
                                     : "target empty load confirmation");
                    previous = expand_host(plan, {}, future, previous).host_return;
                }
                future.depend(*previous, check.return_nodes.at(action.rank));
            }
            continue;
        }
        if (check.storage) state_->drain_prefetch_queues(fact, common.storage);
        else if (check.write) state_->acknowledge_writes(fact, common.writes);
        else state_->acknowledge_loads(fact, common.loads);
        if (observer_)
            observer_({ action.check,
                        action.rank,
                        time,
                        check.storage,
                        common.storage,
                        check.samples.at(action.rank).loads,
                        common.loads,
                        check.storage || check.write ? 0 : state_->load_completion_count(fact),
                        check.write,
                        check.samples.at(action.rank).writes,
                        common.writes,
                        check.write ? state_->write_completion_count(fact) : 0 });
    }
}

} // namespace markov::trace_graph::modules::hicache::runtime
