#include "markov/trace_graph/modules/hicache/runtime/prefetch_waits.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/cost/host_operation.hpp"
#include "markov/trace_graph/source_dag/prefetch.hpp"
#include "markov/trace_graph/structure/cpu_collective.hpp"
#include "markov/trace_graph/structure/hicache/query.hpp"
#include "markov/trace_graph/structure/missing_work.hpp"
#include <algorithm>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {
using Control = PrefetchControlObservation;
using Action = model::PrefetchSchedulerAction;

namespace {

struct ReceiveGroup {
    std::vector<int> members;
    size_t root_index;
    bool operator==(const ReceiveGroup &) const = default;
};

std::optional<ReceiveGroup> observe_receive_group(const core::DagGraph & graph, const CpuCollectiveObservation & collectives,
                                                  const std::vector<int> & participants) {
    std::optional<ReceiveGroup> result;
    for (const auto & round : collectives.rounds) {
        if (round.calls.empty()) continue;
        const auto & event = graph.runtime_observations().at(round.calls.front().observation);
        if (event.arg("role") != "request_receive" || event.arg("operation") != "broadcast") continue;

        auto ordered = round.members;
        std::ranges::sort(ordered);
        if (ordered != participants) continue;

        const auto root = event.arg("group_src").empty()
                              ? static_cast<size_t>(std::ranges::find(round.members, std::stoi(event.arg("src"))) - round.members.begin())
                              : std::stoul(event.arg("group_src"));
        if (root >= round.members.size()) throw std::runtime_error("Source scheduler broadcast root is outside its group");

        ReceiveGroup observed{ round.members, root };
        if (result && *result != observed) throw std::runtime_error("Source scheduler has conflicting request-receive groups");
        result = std::move(observed);
    }
    return result;
}

CpuCollectiveCallTiming collective_cost(cost::OperatorCostProvider & costs, structure::OperationWork work) {
    const auto duration = [&](std::string_view scope) {
        work.timing_scope = scope;
        return costs.estimate(work).duration_us;
    };
    return { .before_submit = duration("before_submit_interval"),
             .submission = duration("submission_service"),
             .dispatch = duration("dispatch_interval"),
             .worker_remainder = duration("worker_remainder"),
             .after_join = duration("after_join_interval") };
}

} // namespace

void HiCachePrefetchWaits::bind(core::DagGraph & graph, model::HiCacheModelReplay & replay, std::string_view source_policy, const std::string & target_policy,
                                const core::ClientRequestChain & chain, cost::OperatorCostProvider & costs, HiCacheQueueConfirmations::WriteWork write_work) {
    if (state_) throw std::logic_error("Prefetch waits are already bound");
    state_ = &replay.state();
    write_work_ = std::move(write_work);
    clients_ = chain.requests;
    const adapters::sglang::HiCacheSourceIndex source(graph);
    const auto rounds = observe_cpu_collectives(source);
    std::set<std::string> requests;
    for (const auto & request : clients_) requests.insert(request.request_id);
    const auto grouped = observe_prefetch_requests(source, rounds, source_policy, &requests);

    // Active checks and retry loops are generated work. Their costs come from
    // independent evidence; source regions only identify replacement sites.
    std::vector<int> participants;
    if (!grouped.empty())
        for (const auto & [rank, calls] : grouped.begin()->second) participants.push_back(rank);
    const auto receive_group = observe_receive_group(graph, rounds, participants);

    using RequestKey = std::pair<std::string, std::string>;
    std::map<RequestKey, const HiCacheFact *> lookups;
    std::map<RequestKey, const HiCacheBatchPathEntry *> inputs;
    for (const auto & item : replay.facts()) {
        if (item.role == HiCacheFactRole::CacheLookupInput) {
            auto & latest = lookups[{ item.fact.pid, item.fact.request_id }];
            if (!latest || item.fact.source_ts > latest->source_ts) latest = &item.fact;
        }
        else if (item.role == HiCacheFactRole::CacheExtendInput)
            for (const auto & path : item.fact.batch_paths) inputs.emplace(RequestKey{ item.fact.pid, path.request_id }, &path);
    }

    std::vector<Control> selected;
    for (const auto & [request, ranks] : grouped)
        for (const auto & [rank, calls] : ranks) selected.insert(selected.end(), calls.begin(), calls.end());
    const auto regions = patch::prepare_prefetch_wait_regions(graph, selected, rounds, observe_prefetch_workers(source, rounds));
    for (const auto & request : clients_) {
        if (!request.generation_reservation_tokens)
            throw structure::MissingWork("Prefill admission needs the workload's declared generation limit, not its measured output length",
                                         {
                                             { "required_observation", "generation_reservation_tokens" },
                                             {           "request_id",              request.request_id }
            });

        const auto & ranks = grouped.at(request.request_id);
        auto wait = std::make_unique<Wait>();
        wait->request = request.request_id;
        std::map<int, uint64_t> migrated_returns;

        for (const auto & [rank, calls] : ranks) {
            const auto * first = source.fact_node(calls.front().progress_fact);
            const auto identity = std::ranges::find_if(replay.facts(), [&](const auto & item) {
                return item.fact.pid == first->pid && item.fact.request_id == request.request_id;
            });
            if (identity == replay.facts().end()) throw std::runtime_error("Progress has no request/cache identity");
            auto fact = identity->fact;
            fact.source_node_id = calls.front().progress_fact;
            fact.source_event_index = first->event_index;
            fact.role = "prefetch_progress_observed";
            fact.source_ts = first->timestamp_us + first->duration_us;
            wait->facts[rank] = std::move(fact);
            const RequestKey key{ first->pid, request.request_id };
            wait->admissions.emplace(rank, state_->prefill_admission(*lookups.at(key), inputs.at(key)->token_count, *request.generation_reservation_tokens));
            // Only this request's unchanged local return can migrate directly.
            // A donor from another source request would be an online cost fit.
            std::optional<uint64_t> local;
            for (const auto & call : calls)
                if (call.local_return) local = source_dag::observe_prefetch_check_cpu(call, source).no_operation_return;
            if (local) migrated_returns.emplace(rank, *local);
            core::TraceEvent interval;
            interval.pid = first->pid;
            interval.tid = first->tid;
            interval.ts = calls.front().cpu.front().cpu.interval_start_us;
            interval.dur = calls.back().cpu.back().cpu.interval_end_us - interval.ts;
            replaced_.push_back(std::move(interval));
        }
        for (const auto & region : regions)
            if (source.fact_node(region.progress_fact)->request_id == request.request_id) {
                graph.mutable_node(region.entry).observe_start = true;
                graph.mutable_node(region.exit).observe_start = true;
                wait->regions.push_back(region);
            }
        auto * stable = wait.get();
        wait->execution = std::make_unique<model::HiCachePrefetchWaitExecution>(
            *state_,
            wait->facts,
            [&costs, participants, receive_group]() {
                if (!receive_group) throw std::runtime_error("Source scheduler lacks request-receive group identity for generated retry work");
                const auto broadcast =
                    structure::cpu_collective_entries(structure::CpuCollectiveKind::Broadcast, receive_group->members, receive_group->root_index);
                const auto all_reduce = structure::cpu_collective_entries(structure::CpuCollectiveKind::AllReduce, participants);
                const auto local = [&](int rank, const std::string & role) {
                    const auto work = structure::hicache::prefetch_scheduler_operation(role, "cpu_service", rank, participants.size());
                    const auto estimate = cost::estimate_host_operation(costs, work, false);
                    return model::PrefetchSchedulerLocalTiming{ estimate.main_us, estimate.main_residual_us };
                };

                model::PrefetchSchedulerTiming body;
                for (const auto action : model::PREFETCH_SCHEDULER_SEQUENCE) {
                    const std::string name(model::PREFETCH_SCHEDULER_NAMES.at(static_cast<size_t>(action)));
                    auto & step = body.steps[action];
                    for (const auto rank : participants) {
                        step.before[rank] = local(rank, name + ".before");
                        auto work = structure::hicache::prefetch_scheduler_operation(name + ".collective", "", rank, participants.size());
                        if (action == Action::Receive) work.features["root_rank"] = receive_group->members.at(receive_group->root_index);
                        auto communication = collective_cost(costs, std::move(work));
                        communication.entry_ranks = (action == Action::Receive ? broadcast : all_reduce).at(rank);
                        step.communication.calls.emplace(rank, std::move(communication));
                        if (action != Action::WriteCompletion) step.after[rank] = local(rank, name + ".after");
                    }
                }
                for (const auto rank : participants) body.tail[rank] = local(rank, "tail");
                return body;
            },
            target_policy,
            [&costs, migrated_returns = std::move(migrated_returns), count = ranks.size()](int rank, std::string_view role) {
                if (role == "no_operation_return") {
                    const auto found = migrated_returns.find(rank);
                    if (found != migrated_returns.end()) return found->second;
                }

                return costs.estimate(structure::hicache::prefetch_check_interval(role, rank, count)).duration_us;
            },
            [&costs, count = ranks.size()](int rank, bool completion) {
                return collective_cost(costs, structure::hicache::prefetch_check_collective(completion, rank, count, ""));
            },
            [this, stable](const model::PrefetchSchedulerBoundary & boundary, uint64_t time, size_t node, simulation::FutureDag & future) {
                scheduler_action(*stable, boundary, time, node, future);
            },
            [this, stable](int rank) {
                const auto admitted = state_->can_admit_prefill(stable->admissions.at(rank));
                stable->rejected_admissions[rank] = !admitted;
                return admitted;
            });
        waits_.push_back(std::move(wait));
    }
}

void HiCachePrefetchWaits::scheduler_action(Wait & wait, const model::PrefetchSchedulerBoundary & boundary, uint64_t time, size_t node,
                                            simulation::FutureDag & future) {
    const auto action = boundary.action;
    if (!active_http_ || *active_http_ != wait.request) throw std::runtime_error("Scheduler wait is outside its active serial HTTP request");
    // The next serial HTTP request cannot arrive until this one completes.
    // A rejected admission did run match_prefix before returning to receive.
    // Its split can change the pending write's publication work, even though
    // no device allocation was admitted. The non-forward scheduler interval
    // covers this local CPU work only as a coarse independent approximation.
    if (action == Action::Receive) {
        const auto rejected = wait.rejected_admissions.find(boundary.rank);
        if (!boundary.apply && rejected != wait.rejected_admissions.end() && rejected->second) {
            state_->match_rejected_prefill(wait.admissions.at(boundary.rank));
            rejected->second = false;
            ++admission_retry_rank_calls_;
        }
        return;
    }
    auto fact = wait.facts.at(boundary.rank);
    fact.ts = time;
    fact.execution_anchor_node_id = node;
    auto & samples = wait.samples[boundary.action];
    if (!boundary.apply) {
        samples[boundary.rank] = action == Action::LoadCompletion ? state_->load_completion_count(fact) : state_->write_completion_count(fact);
        return;
    }
    if (samples.size() != wait.facts.size()) throw std::runtime_error("Wait confirmation returned before all local samples");
    const auto count = std::ranges::min_element(samples, {}, [](const auto & pair) { return pair.second; })->second;
    if (action == Action::LoadCompletion) state_->acknowledge_loads(fact, count);
    else if (action == Action::WriteCompletion) {
        if (const auto returned = write_work_({ fact, count }, future)) future.depend(*returned, boundary.continuation);
    }
    else throw std::runtime_error("Storage drain must be handled by the shared wait executor");
}

void HiCachePrefetchWaits::start(simulation::FutureDag & future) {
    for (auto & wait : waits_) {
        CpuRankNodes entries;
        for (const auto & region : wait->regions) entries[region.rank] = region.entry;
        const auto returned = wait->execution->start(entries, future);
        for (const auto & region : wait->regions) future.depend(returned.at(region.rank), region.exit);
    }
}

void HiCachePrefetchWaits::advance(size_t node, uint64_t time, simulation::FutureDag & future) {
    for (const auto & request : clients_) {
        if (node == request.start) {
            if (active_http_) throw std::runtime_error("Live wait requires serial HTTP requests");
            active_http_ = request.request_id;
        }
        if (node == request.completion) {
            if (active_http_ != request.request_id) throw std::runtime_error("HTTP completion does not match active request");
            active_http_.reset();
        }
    }
    for (auto & wait : waits_) wait->execution->advance(node, time, future);
}

} // namespace markov::trace_graph::modules::hicache::runtime
