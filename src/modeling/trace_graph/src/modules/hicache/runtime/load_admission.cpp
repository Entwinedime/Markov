#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheWriteCalls::prepare_generated_load_admissions(
    const patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues) {
    if (load_index_calibration_.empty()) return;
    std::ifstream file(load_index_calibration_);
    if (!file) throw std::runtime_error("Cannot read independent load index calibration");
    const auto document = nlohmann::json::parse(file);
    if (document.at("role") != "fixed_calibration" || document.at("operation") != "load_index"
        || document.at("source_manifest").get<std::string>().empty())
        throw std::runtime_error("Load index costs require independent calibration provenance");
    // The simulator uses integer microseconds. Round measured sample means,
    // retaining every cost component rather than dropping sub-microsecond work.
    const auto duration = [](const nlohmann::json & value) {
        const auto cost = value.get<double>();
        if (!std::isfinite(cost) || cost < 0 || cost >= std::ldexp(1.0, 64))
            throw std::invalid_argument("Invalid load index cost");
        return static_cast<uint64_t>(std::floor(cost + 0.5));
    };
    const auto & graph = source.graph();
    for (const auto & branch : load_branches_) {
        const auto key = std::pair{branch.envelope.pid, branch.envelope.tid};
        if (generated_load_admissions_.contains(key)) continue;
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
                && (donor.region.kind == HiCacheEvictionRegion::Kind::ReleaseRegular
                    || donor.region.kind == HiCacheEvictionRegion::Kind::ReleaseBackup)
                && donor.expansion.streams.size() == 1) witness(donor.expansion.streams.front().source_node);
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
            return generate_load_index_operation(graph, branch.entry_node, resources->worker, resources->compute,
                {duration(cost.at("main_us")), duration(cost.at("main_residual_us")), duration(cost.at("worker_us")),
                 duration(cost.at("dispatch_us")), duration(cost.at("device_us"))}, name);
        };
        GeneratedLoadAdmission program;
        for (const auto * name : {"multiply", "range", "add"}) program.allocation.push_back(operation(name));
        program.clone = operation("clone");
        program.lanes.push_back(graph.node(*compute).lane_id);
        std::pair<double, double> remainder{};
        program.tail = generated_cpu_control(graph.node_lane_key(branch.entry_node),
            duration(row->at("tail_us")), duration(row->at("tail_residual_us")), remainder,
            "target load admission bookkeeping");
        generated_load_admissions_.emplace(key, std::move(program));
    }
}

void HiCacheWriteCalls::prepare_load_admissions(const patch::HiCacheSourceDagIndex & source,
                                               const simulation::detail::CpuTaskQueues & queues) {
    const auto lanes = [&](const HiCacheHostExpansion & plan) {
        std::vector<size_t> result;
        for (const auto & stream : plan.streams) result.push_back(source.graph().node(stream.source_node).lane_id);
        for (const auto & wait : plan.waits) result.push_back(source.graph().node(wait.source_node).lane_id);
        return result;
    };
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
            if (fact.fact_role == "loadback_decision_observed" && fact.phase == "end"
                && fact.pid == first.pid && fact.tid == first.tid && fact.request_id == branch.condition.arg("request_id")
                && fact.timestamp_us <= first.ts && fact.timestamp_us + fact.duration_us >= last.ts + last.dur) {
                if (admission) throw std::runtime_error("Load template has ambiguous source admission geometry");
                admission = &fact;
            }
        if (!admission || !admission->effective_token_count) throw std::runtime_error("Successful load template lacks its source token count");
        const auto end_us = [](const auto & e) { return (e.ts * 1000 + e.ts_submicro_ns + e.dur * 1000 + e.dur_submicro_ns) / 1000; };
        const auto region = [&](uint64_t begin, uint64_t end) {
            return prepare_host_expansion(source, queues, observe_host_template(source, first.pid, first.tid, begin, end), "host load: ");
        };
        LoadAdmissionTemplate donor{ .pid = first.pid, .tid = first.tid, .tokens = admission->effective_token_count,
            .prefix = region(end_us(branch.condition), first.ts),
            .allocation = region(last.ts, last.ts + last.dur),
            .suffix = region(last.ts + last.dur, end_us(branch.envelope)) };
        donor.prefix_lanes = lanes(donor.prefix);
        donor.allocation_lanes = lanes(donor.allocation);
        donor.suffix_lanes = lanes(donor.suffix);
        if (first.arg("allocated") == "false") {
            const patch::HiCacheSourceFactNode * eviction = nullptr;
            for (const auto & fact : source.fact_nodes())
                if (fact.fact_role == "capacity_result_observed" && fact.phase == "end" && fact.pid == first.pid && fact.tid == first.tid
                    && fact.timestamp_us >= first.ts + first.dur && fact.timestamp_us + fact.duration_us <= last.ts) {
                    if (eviction) throw std::runtime_error("Load retry has multiple source capacity envelopes");
                    eviction = &fact;
                }
            if (!eviction) throw std::runtime_error("Load retry lacks its source capacity envelope");
            auto failure = region(first.ts, eviction->timestamp_us);
            if (failure.streams.empty() && failure.waits.empty() && failure.event_waits.empty()) donor.failure = std::move(failure);
            if (eviction->timestamp_us + eviction->duration_us < last.ts) {
                donor.retry = region(eviction->timestamp_us + eviction->duration_us, last.ts);
                donor.retry_lanes = lanes(*donor.retry);
            }
        }
        load_admissions_.push_back(std::move(donor));
    }
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
    const auto tokens = work->requested_pages * page_size_;
    size_t selected = load_admissions_.size();
    uint64_t nearest = UINT64_MAX;
    for (size_t i = 0; i < load_admissions_.size(); ++i) {
        // Cost coverage selects the generator, not whether this base happened
        // to execute a successful allocation. Retry and clone cardinality are
        // properties of target state, including when a donor is available.
        if (!load_index_calibration_.empty()) break;
        const auto & donor = load_admissions_[i];
        if (donor.pid != item.fact.pid || donor.tid != item.fact.tid) continue;
        const auto distance = donor.tokens > tokens ? donor.tokens - tokens : tokens - donor.tokens;
        if (selected == load_admissions_.size() || distance < nearest) { selected = i; nearest = distance; }
    }
    if (selected == load_admissions_.size()) {
        const auto generated = generated_load_admissions_.find({item.fact.pid, item.fact.tid});
        if (generated == generated_load_admissions_.end())
            throw std::runtime_error("New load admission lacks independent index costs or scheduler resource evidence");
        auto & call = capacity_calls_.at(branch.owner);
        const auto & program = generated->second;
        const bool allocated = replay_.state().load_allocation_will_succeed(item.fact);
        std::optional<size_t> previous;
        if (!allocated || replay_.state().eviction_work(item.fact)) {
            // No-sort alloc checks the available count and returns None without
            // device work. Its unobserved CPU cost uses this base's no-load
            // control as an explicit proxy, not a fabricated measured value.
            double cpu = 0, residual = 0;
            for (const auto & part : branch.original.nodes) {
                cpu += part.work.duration;
                residual += part.work.cpu_gap_after;
            }
            std::pair<double, double> remainder{};
            load_failure_templates_.insert_or_assign(branch.owner, generated_cpu_control(call.lane, cpu, residual, remainder,
                "target failed allocation: base no-load control cost proxy"));
        }
        if (replay_.state().eviction_work(item.fact)) {
            if (allocator_need_sort_) throw std::runtime_error("Generated load allocation does not yet model free-page merge/sort work");
            auto fact = item.fact;
            fact.ts = time; fact.execution_anchor_node_id = node;
            previous = expand_capacity(call, fact, future);
            if (!previous) throw std::logic_error("Target load retry has no capacity return");
            allocation_returns_.erase(*previous);
        }
        for (const auto lane : program.lanes)
            if (!call.positions.contains(lane)) throw std::runtime_error("Generated load lacks its compute insertion position");
        HiCacheHostSequence allocation(call.positions, stream_insertions_, future, previous);
        size_t returned = node;
        if (allocated) {
            for (const auto & operation : program.allocation)
                returned = allocation.append(operation, program.lanes).host_return;
        } else {
            returned = allocation.append(load_failure_templates_.at(branch.owner), {}).host_return;
        }
        future.depend(returned, branch.continuation);
        allocation_returns_[returned].push_back(branch.owner);
        load_branch_returns_.emplace(returned, LoadBranchReturn{branch.owner, std::nullopt, branch.continuation, allocated});
        return;
    }
    const auto & donor = load_admissions_[selected];
    auto & call = capacity_calls_.at(branch.owner);
    for (const auto * lanes : { &donor.prefix_lanes, &donor.allocation_lanes, &donor.suffix_lanes })
        for (const auto lane : *lanes)
            if (!call.positions.contains(lane)) throw std::runtime_error("New load stream position is unavailable: " + capacity_position_errors_.at(branch.owner).at(lane));
    HiCacheHostSequence prefix(call.positions, stream_insertions_, future);
    auto previous = prefix.append(donor.prefix, donor.prefix_lanes).host_return;
    auto fact = item.fact;
    fact.ts = time; fact.execution_anchor_node_id = node;
    if (replay_.state().eviction_work(fact)) {
        const LoadAdmissionTemplate * retry_donor = donor.failure ? &donor : nullptr;
        for (const auto & candidate : load_admissions_)
            if (!retry_donor && candidate.pid == fact.pid && candidate.tid == fact.tid && candidate.failure) retry_donor = &candidate;
        if (!retry_donor) throw std::runtime_error("New load capacity path lacks a base failed allocation");
        load_failure_templates_.insert_or_assign(branch.owner, *retry_donor->failure);
        previous = *expand_capacity(call, fact, future, previous);
        // Capacity release alone cannot publish allocation. The successful
        // allocator call must return before state and prefix work proceed.
        allocation_returns_.erase(previous);
        if (retry_donor->retry) {
            HiCacheHostSequence retry(call.positions, stream_insertions_, future, previous);
            previous = retry.append(*retry_donor->retry, retry_donor->retry_lanes).host_return;
        }
    }
    HiCacheHostSequence allocation(call.positions, stream_insertions_, future, previous);
    const auto returned = allocation.append(donor.allocation, donor.allocation_lanes).host_return;
    future.depend(returned, branch.continuation);
    allocation_returns_[returned].push_back(branch.owner);
    load_branch_returns_.emplace(returned, LoadBranchReturn{branch.owner, selected, branch.continuation});
}

} // namespace markov::trace_graph::modules::hicache::runtime
