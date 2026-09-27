#include "markov/trace_graph/modules/hicache/runtime/control_calibration.hpp"
#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheWriteCalls::split_load_tail(const core::DagGraph & graph, size_t main, LoadAdmissionProgram & program) {
    const auto & source = program.tail.work;
    if (source.streams.size() != 1 || !source.waits.empty() || !source.event_waits.empty()) return;

    std::vector<size_t> submissions;
    for (size_t i = 0; i < source.nodes.size(); ++i) {
        const auto & node = source.nodes[i];
        if (!node.work.is_cpu || node.queue_member) continue;
        const auto & name = node.work.name;
        if (name.find("Enqueue@") == std::string::npos) continue;
        if (!name.ends_with("Enqueue@aclnnInplaceCopy")) return;
        submissions.push_back(i);
    }
    if (submissions.empty()) return;

    LoadIndexOperationCost total;
    std::optional<size_t> worker;
    std::set<size_t> devices;
    size_t previous = 0;
    const auto add = [](uint64_t & sum, uint64_t value) { sum = core::checked_add_u64(sum, value, "Load clone cost overflow"); };
    for (const auto submission : submissions) {
        std::optional<size_t> job, device;
        for (size_t i = 0; i < source.nodes.size(); ++i)
            if (source.nodes[i].submission == submission) {
                if (job) return;
                job = i;
            }
        if (!job || !source.nodes[*job].queue_member) return;
        for (const auto & edge : source.edges)
            if (edge.from == *job && !source.nodes[edge.to].work.is_cpu) {
                if (device && *device != edge.to) return;
                device = edge.to;
            }
        if (!device || !devices.insert(*device).second) return;

        const auto member = *source.nodes[*job].queue_member;
        if (worker && graph.node(*worker).lane_id != graph.node(member).lane_id) return;
        worker = member;
        for (size_t i = previous; i <= submission; ++i) {
            if (!source.nodes[i].work.is_cpu || source.nodes[i].queue_member) return;
            add(total.main_us, source.nodes[i].work.duration);
            add(total.main_residual_us, source.nodes[i].work.cpu_gap_after);
        }
        add(total.worker_us, source.nodes[*job].work.duration);
        add(total.dispatch_us, source.nodes[*job].work.cpu_task_ready_delay_us.value());
        add(total.device_us, source.nodes[*device].work.duration);
        previous = submission + 1;
    }
    // Do not silently drop a different device primitive or unowned worker.
    if (devices.size() != std::ranges::count_if(source.nodes, [](const auto & node) { return !node.work.is_cpu; })
        || submissions.size() != std::ranges::count_if(source.nodes, [](const auto & node) { return node.queue_member.has_value(); }))
        return;

    uint64_t tail_us = 0, residual_us = 0;
    for (size_t i = previous; i < source.nodes.size(); ++i) {
        const auto & node = source.nodes[i];
        if (!node.work.is_cpu || node.queue_member) continue;
        add(tail_us, node.work.duration);
        add(residual_us, node.work.cpu_gap_after);
    }
    // A source-only per-call mean, not a target fit. Keep worker dispatch and
    // residual CPU separate; size-dependent clone costs remain an extrapolation.
    const auto count = submissions.size();
    for (const auto field : { &LoadIndexOperationCost::main_us, &LoadIndexOperationCost::main_residual_us,
                              &LoadIndexOperationCost::worker_us, &LoadIndexOperationCost::dispatch_us, &LoadIndexOperationCost::device_us }) {
        auto & value = total.*field;
        value = value / count + (value % count >= (count + 1) / 2);
    }
    const auto compute = source.streams.front().source_node;
    program.clone = LoadStep{ generate_load_index_operation(graph, main, *worker, compute, total, "clone: base per-call cost"),
                              { graph.node(compute).lane_id } };
    std::pair<double, double> remainder{};
    program.tail = LoadStep{ generated_cpu_control(graph.node_lane_key(main), tail_us, residual_us, remainder,
                                                   "target load admission bookkeeping: base cost"), {} };
}

void HiCacheWriteCalls::prepare_generated_load_admissions(const patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues) {
    if (load_index_calibration_.empty()) return;
    const auto document = read_control_calibration(load_index_calibration_, "load_index");
    const auto & graph = source.graph();
    for (const auto & branch : load_branches_) {
        const auto key = std::pair{ branch.envelope.pid, branch.envelope.tid };
        if (std::ranges::any_of(load_admissions_, [&](const auto & sample) { return sample.pid == key.first && sample.tid == key.second; })) continue;
        std::optional<size_t> compute;
        const auto witness = [&](size_t node) {
            if (compute && graph.node(*compute).lane_id != graph.node(node).lane_id)
                throw std::runtime_error("Scheduler load admission has conflicting compute roles");
            compute = node;
        };
        for (const auto & [id, donor] : templates_)
            if (donor.pid == key.first && donor.tid == key.second) witness(donor.compute_record);
        for (const auto & donor : eviction_controls_)
            if (donor.pid == key.first && donor.tid == key.second
                && (donor.region.kind == HiCacheEvictionRegion::Kind::ReleaseRegular || donor.region.kind == HiCacheEvictionRegion::Kind::ReleaseBackup)
                && donor.expansion.streams.size() == 1)
                witness(donor.expansion.streams.front().source_node);
        if (!compute) continue; // Required only if this target needs a new branch.
        const auto resources = observe_load_index_resources(source, queues, branch.entry_node, *compute);
        if (!resources) continue;
        const auto rank = graph.node(branch.entry_node).gpu_id;
        const nlohmann::json * row = nullptr;
        for (const auto & sample : document.at("ranks"))
            if (sample.at("rank").get<int>() == rank) {
                if (row) throw std::invalid_argument("Duplicate load index calibration rank");
                row = &sample;
            }
        if (!row) continue;
        const auto operation = [&](const char * name) {
            const auto & cost = row->at("operations").at(name);
            return LoadStep{ generate_load_index_operation(graph, branch.entry_node, resources->worker, resources->compute, read_load_operation_cost(cost), name),
                             { graph.node(*compute).lane_id } };
        };
        LoadAdmissionProgram program{ .pid = key.first, .tid = key.second };
        for (const auto * name : { "multiply", "range", "add" }) program.allocation.push_back(operation(name));
        program.clone = operation("clone");
        std::pair<double, double> remainder{};
        program.tail.work = generated_cpu_control(graph.node_lane_key(branch.entry_node),
                                             read_control_duration(row->at("tail_us")),
                                             read_control_duration(row->at("tail_residual_us")),
                                             remainder,
                                             "target load admission bookkeeping");
        load_admissions_.push_back(std::move(program));
    }
}

void HiCacheWriteCalls::prepare_load_admissions(const patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues) {
    if (!load_index_calibration_.empty()) {
        prepare_generated_load_admissions(source, queues);
        return;
    }

    for (const auto & branch : load_branches_) {
        if (branch.condition.arg("needed") != "true") continue;
        std::vector<const LoadAttempt *> attempts;
        for (const auto & attempt : load_attempts_)
            if (attempt.owner == branch.owner) attempts.push_back(&attempt);
        std::ranges::sort(attempts, {}, [](const auto * attempt) { return attempt->event.ts; });
        if (attempts.empty() || attempts.back()->event.arg("allocated") != "true") continue;
        const auto & first = attempts.front()->event;
        const auto & last = attempts.back()->event;
        const patch::HiCacheSourceFactNode * admission = nullptr;
        for (const auto & fact : source.fact_nodes())
            if (fact.fact_role == "loadback_decision_observed" && fact.phase == "end" && fact.pid == first.pid && fact.tid == first.tid
                && fact.request_id == branch.condition.arg("request_id") && fact.timestamp_us <= first.ts
                && fact.timestamp_us + fact.duration_us >= last.ts + last.dur) {
                if (admission) throw std::runtime_error("Load template has ambiguous source admission geometry");
                admission = &fact;
            }
        if (!admission || !admission->effective_token_count) throw std::runtime_error("Successful load template lacks its source token count");
        const auto end_us = [](const auto & e) { return (e.ts * 1'000 + e.ts_submicro_ns + e.dur * 1'000 + e.dur_submicro_ns) / 1'000; };
        const auto region = [&](uint64_t begin, uint64_t end) {
            auto work = prepare_host_expansion(source, queues, observe_host_template(source, first.pid, first.tid, begin, end), "host load: ");
            auto lanes = work.resource_lanes(source.graph());
            return LoadStep{ std::move(work), std::move(lanes) };
        };
        LoadAdmissionProgram donor{ .pid = first.pid,
                                     .tid = first.tid,
                                     .tokens = admission->effective_token_count,
                                     .prefix = region(end_us(branch.condition), first.ts),
                                     .allocation = { region(last.ts, last.ts + last.dur) },
                                     .tail = region(last.ts + last.dur, end_us(branch.envelope)) };
        split_load_tail(source.graph(), branch.entry_node, donor);
        if (first.arg("allocated") == "false") {
            const patch::HiCacheSourceFactNode * eviction = nullptr;
            for (const auto & fact : source.fact_nodes())
                if (fact.fact_role == "capacity_result_observed" && fact.phase == "end" && fact.pid == first.pid && fact.tid == first.tid
                    && fact.timestamp_us >= first.ts + first.dur && fact.timestamp_us + fact.duration_us <= last.ts) {
                    if (eviction) throw std::runtime_error("Load retry has multiple source capacity envelopes");
                    eviction = &fact;
                }
            if (!eviction) throw std::runtime_error("Load retry lacks its source capacity envelope");
            auto failure = region(first.ts, eviction->timestamp_us).work;
            if (failure.streams.empty() && failure.waits.empty() && failure.event_waits.empty()) donor.failure = std::move(failure);
            if (eviction->timestamp_us + eviction->duration_us < last.ts) {
                donor.retry = region(eviction->timestamp_us + eviction->duration_us, last.ts);
            }
        }
        load_admissions_.push_back(std::move(donor));
    }
}

size_t HiCacheWriteCalls::select_load_admission(const std::string & pid, const std::string & tid, uint64_t tokens) const {
    size_t selected = load_admissions_.size();
    uint64_t nearest = UINT64_MAX;
    for (size_t i = 0; i < load_admissions_.size(); ++i) {
        const auto & sample = load_admissions_[i];
        if (sample.pid != pid || sample.tid != tid) continue;
        if (!sample.tokens) return i;

        // A decomposed sample describes target clone cardinality. An opaque
        // aggregate remains usable only when no such source evidence exists.
        if (selected != load_admissions_.size() && load_admissions_[selected].clone && !sample.clone) continue;

        const auto distance = *sample.tokens > tokens ? *sample.tokens - tokens : tokens - *sample.tokens;
        if (selected == load_admissions_.size() || (!load_admissions_[selected].clone && sample.clone) || distance < nearest) {
            selected = i;
            nearest = distance;
        }
    }
    if (selected == load_admissions_.size())
        throw std::runtime_error("Load admission lacks base or independent costs on its scheduler resources");
    return selected;
}

void HiCacheWriteCalls::bind_load_resources(const patch::HiCacheSourceDagIndex & source, Call & call, const LoadAdmissionProgram & program) {
    for (const auto * step : { &program.prefix, &program.retry, &program.clone })
        if (*step) bind_call_resources(source, call, (*step)->work, true);
    for (const auto & step : program.allocation) bind_call_resources(source, call, step.work, true);
    bind_call_resources(source, call, program.tail.work, true);
}

void HiCacheWriteCalls::start_load_branch(size_t node, uint64_t time, simulation::FutureDag & future) {
    const auto & branch = empty_load_branches_.at(node);
    const auto & item = replay_.fact(branch.owner);
    if (!item.started) throw std::runtime_error("Host-load branch precedes its lookup input");
    const auto * work = replay_.state().load_admission_work(item.fact);
    if (!work) {
        const auto restored = expand_host(branch.original, {}, future);
        future.depend(restored.host_return, branch.continuation);
        return;
    }
    const auto selected = select_load_admission(item.fact.pid, item.fact.tid, work->requested_pages * page_size_);
    const auto & program = load_admissions_[selected];
    auto & call = capacity_calls_.at(branch.owner);
    auto fact = item.fact;
    fact.ts = time;
    fact.execution_anchor_node_id = node;

    // Aggregate source evidence only describes a successful allocation. Keep
    // that limitation explicit; it does not supply a failed-return cost.
    const bool allocated = replay_.state().load_allocation_will_succeed(fact);
    if (!allocated && program.tokens)
        throw std::runtime_error("Aggregate base load cost does not cover allocation failure");
    const bool evict = replay_.state().eviction_work(fact) != nullptr;
    const LoadAdmissionProgram * retry_cost = nullptr;
    if (evict || !allocated) {
        if (program.tokens) {
            retry_cost = program.failure ? &program : nullptr;
            for (const auto & sample : load_admissions_)
                if (!retry_cost && sample.pid == fact.pid && sample.tid == fact.tid && sample.failure) retry_cost = &sample;
            if (!retry_cost) throw std::runtime_error("New load capacity path lacks a base failed allocation");
            load_failure_templates_.insert_or_assign(branch.owner, *retry_cost->failure);
        }
        else {
            // No-sort failure does no device work. Retain the existing,
            // explicitly uncertain no-load CPU proxy; it is not a measurement.
            double cpu = 0, residual = 0;
            for (const auto & part : branch.original.nodes) {
                cpu += part.work.duration;
                residual += part.work.cpu_gap_after;
            }
            std::pair<double, double> remainder{};
            load_failure_templates_.insert_or_assign(
                branch.owner, generated_cpu_control(call.lane, cpu, residual, remainder, "target failed allocation: base no-load control cost proxy"));
        }
    }

    std::optional<size_t> previous;
    if (program.prefix) {
        HiCacheHostSequence prefix(call.positions, stream_insertions_, future);
        previous = prefix.append(program.prefix->work, program.prefix->lanes).host_return;
    }
    if (evict) {
        if (!program.tokens && allocator_need_sort_) throw std::runtime_error("Generated load allocation does not yet model free-page merge/sort work");
        previous = expand_capacity(call, fact, future, previous);
        if (!previous) throw std::logic_error("Target load retry has no capacity return");
        // Release completion is not allocation completion: publish only after
        // the allocator below returns, for every source of operation costs.
        allocation_returns_.erase(*previous);
        if (retry_cost && retry_cost->retry) {
            HiCacheHostSequence retry(call.positions, stream_insertions_, future, previous);
            previous = retry.append(retry_cost->retry->work, retry_cost->retry->lanes).host_return;
        }
    }

    HiCacheHostSequence allocation(call.positions, stream_insertions_, future, previous);
    auto returned = node;
    if (allocated) {
        for (const auto & step : program.allocation) returned = allocation.append(step.work, step.lanes).host_return;
    }
    else returned = allocation.append(load_failure_templates_.at(branch.owner), {}).host_return;

    future.depend(returned, branch.continuation);
    allocation_returns_[returned].push_back(branch.owner);
    load_branch_returns_.emplace(returned, LoadBranchReturn{ branch.owner, selected, branch.continuation, allocated });
}

} // namespace markov::trace_graph::modules::hicache::runtime
