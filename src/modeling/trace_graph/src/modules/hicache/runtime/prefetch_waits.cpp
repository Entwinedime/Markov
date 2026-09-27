#include "markov/trace_graph/modules/hicache/runtime/prefetch_waits.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_wait_calibration.hpp"
#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {
using Control = PrefetchControlObservation;
using Action = model::PrefetchSchedulerAction;

void HiCachePrefetchWaits::bind(core::DagGraph & graph, model::HiCacheModelReplay & replay, std::string_view source_policy, const std::string & target_policy,
                                const core::ClientRequestChain & chain, PrefetchWaitObserver observer, const std::string & calibration_path,
                                const std::vector<std::string> & cpu_calibrations) {
    if (state_) throw std::logic_error("Prefetch waits are already bound");
    state_ = &replay.state();
    clients_ = chain.requests;
    observer_ = std::move(observer);
    const patch::HiCacheSourceDagIndex source(graph);
    const auto rounds = observe_cpu_collectives(source);
    const auto workers = observe_prefetch_workers(source, rounds);
    std::set<std::string> requests;
    for (const auto & request : clients_) requests.insert(request.request_id);
    const auto grouped = observe_prefetch_requests(source, rounds, source_policy, &requests);

    // First complete active request in HTTP order; not a donor selected by
    // prediction error. Only branch work becomes a template, never page counts.
    model::PrefetchCheckTiming timing;
    model::PrefetchSchedulerTiming body;
    for (const auto & request : clients_) {
        const auto at = grouped.find(request.request_id);
        if (at == grouped.end()) throw std::runtime_error("Selected HTTP request has no progress observation");
        const auto measured = target_policy == "best_effort" ? model::observe_prefetch_stop_timing(source, rounds, at->second)
                                                             : model::observe_prefetch_wait_timing(source, rounds, workers, at->second);
        if (!measured) continue;
        timing = measured->check;
        body = measured->scheduler;
        template_ = { .request_id = request.request_id, .cpu = timing.cpu };
        break;
    }
    if (template_.request_id.empty() && !calibration_path.empty()) {
        const auto calibrated = model::read_prefetch_wait_calibration(calibration_path);
        if ((target_policy == "best_effort") != (calibrated.source_policy == "best_effort"))
            throw std::runtime_error("Independent prefetch template does not cover the target stop policy");
        timing = calibrated.timing.check;
        body = calibrated.timing.scheduler;
        template_ = { .request_id = calibrated.request_id, .cpu = timing.cpu, .calibration_manifest = calibrated.source_manifest };
    }
    // An inactive target needs only its local-return cost. Keep active branch
    // timings absent; the executed check requests them if a prefetch is ongoing.
    if (timing.cpu.empty())
        for (const auto & [request, ranks] : grouped)
            for (const auto & [rank, calls] : ranks) timing.cpu.try_emplace(rank);
    for (const auto & [request, ranks] : grouped)
        if (ranks.size() != timing.cpu.size() || std::ranges::any_of(ranks, [&](const auto & item) { return !timing.cpu.contains(item.first); }))
            throw std::runtime_error("Wait timing rank membership differs from base");

    std::vector<Control> selected;
    for (const auto & [request, ranks] : grouped)
        for (const auto & [rank, calls] : ranks) selected.insert(selected.end(), calls.begin(), calls.end());
    const auto regions = patch::prepare_prefetch_wait_regions(graph, selected, rounds, workers);
    // First observed local return per rank, in the same HTTP order used below.
    // This evidence is shared by requests, not rescanned for every request.
    std::map<int, uint64_t> base_local;
    for (const auto & request : clients_)
        for (const auto & [rank, calls] : grouped.at(request.request_id)) {
            if (base_local.contains(rank)) continue;
            for (const auto & call : calls)
                if (call.local_return) {
                    if (const auto cost = model::observe_prefetch_check_cpu(call, &source).no_operation_return) base_local.emplace(rank, *cost);
                    break;
                }
        }
    auto local_sources = cpu_calibrations;
    if (!calibration_path.empty()) local_sources.insert(local_sources.begin(), calibration_path);
    std::map<std::string, model::PrefetchWaitCalibration> shared_cpu;
    for (const auto & request : clients_) {
        const auto & ranks = grouped.at(request.request_id);
        auto wait = std::make_unique<Wait>();
        wait->request = request.request_id;
        auto local_timing = timing;
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
            // Prefer this request's own local-return cost, then another
            // observed local return from this base/rank. Missing is not zero.
            std::optional<uint64_t> local;
            for (const auto & call : calls)
                if (call.local_return) local = model::observe_prefetch_check_cpu(call, &source).no_operation_return;
            if (!local && base_local.contains(rank)) local = base_local.at(rank);
            for (const auto & path : local_sources) {
                if (local) break;
                auto found = shared_cpu.find(path);
                if (found == shared_cpu.end()) found = shared_cpu.emplace(path, model::read_prefetch_wait_calibration(path)).first;
                const auto at = found->second.timing.check.cpu.find(rank);
                if (at == found->second.timing.check.cpu.end()) continue;
                local = at->second.no_operation_return;
                if (local) template_.local_return_calibration_manifests[rank] = found->second.source_manifest;
            }
            local_timing.cpu.at(rank).no_operation_return = local;
            core::TraceEvent interval;
            interval.pid = first->pid;
            interval.tid = first->tid;
            interval.ts = calls.front().cpu.front().cpu.interval_start_us;
            interval.dur = calls.back().cpu.back().cpu.interval_end_us - interval.ts;
            replaced_.push_back(std::move(interval));
        }
        for (const auto & region : regions)
            if (source.fact_node(region.progress_fact)->request_id == request.request_id) wait->regions.push_back(region);
        auto * stable = wait.get();
        wait->execution = std::make_unique<model::HiCachePrefetchWaitExecution>(
            *state_,
            wait->facts,
            local_timing,
            body,
            target_policy,
            [this, stable](Action action, const model::PrefetchSchedulerBoundary & boundary, uint64_t time, simulation::FutureDag &) {
                scheduler_action(*stable, action, boundary, time);
            });
        waits_.push_back(std::move(wait));
    }
}

void HiCachePrefetchWaits::scheduler_action(Wait & wait, Action action, const model::PrefetchSchedulerBoundary & boundary, uint64_t time) {
    if (!active_http_ || *active_http_ != wait.request) throw std::runtime_error("Scheduler wait is outside its active serial HTTP request");
    if (action == Action::Receive) {
        // The next serial HTTP request cannot arrive until this one completes.
        if (boundary.apply && observer_.scheduler) observer_.scheduler({ wait.request, boundary.rank, time, action });
        return;
    }
    auto fact = wait.facts.at(boundary.rank);
    fact.ts = time;
    auto & samples = wait.samples[boundary.step];
    if (!boundary.apply) {
        samples[boundary.rank] = action == Action::LoadCompletion ? state_->load_completion_count(fact) : state_->write_completion_count(fact);
        return;
    }
    if (samples.size() != wait.facts.size()) throw std::runtime_error("Wait confirmation returned before all local samples");
    const auto count = std::ranges::min_element(samples, {}, [](const auto & pair) { return pair.second; })->second;
    if (action == Action::LoadCompletion) state_->acknowledge_loads(fact, count);
    else if (action == Action::WriteCompletion) state_->acknowledge_writes(fact, count);
    else throw std::runtime_error("Storage drain must be handled by the shared wait executor");
    if (observer_.scheduler) observer_.scheduler({ wait.request, boundary.rank, time, action, count });
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
    for (auto & wait : waits_)
        if (const auto rank = wait->execution->advance(node, time, future)) {
            const auto * operation = state_->prefetch_operation(wait->facts.at(*rank));
            if (observer_.returned) {
                PrefetchWaitReturn row{ wait->request,
                                        *rank,
                                        time,
                                        wait->execution->checks_issued(),
                                        operation ? operation->completed_pages.size() : 0,
                                        operation && operation->execution_stop_ts.has_value() };
                if (operation) {
                    row.query_return_us = operation->query_return_ts;
                    if (operation->payload_transfer_issued && operation->io_schedule.start_ts <= time) row.io_start_us = operation->io_schedule.start_ts;
                    for (const auto & batch : operation->io_schedule.batches)
                        for (const auto at : batch.page_ready_ts) {
                            if (at > time) continue;
                            if (!row.first_page_us || at < *row.first_page_us) row.first_page_us = at;
                            if (!row.last_page_us || at > *row.last_page_us) row.last_page_us = at;
                        }
                    row.stop_us = operation->execution_stop_ts;
                    if (operation->target_boundary_ts) row.visible_us = operation->target_boundary_ts;
                    row.worker_return_us = operation->io_return_ts;
                }
                observer_.returned(row);
            }
        }
}

} // namespace markov::trace_graph::modules::hicache::runtime
