#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/cost/hicache_release.hpp"
#include "markov/trace_graph/cost/missing_evidence.hpp"
#include "markov/trace_graph/modules/hicache/runtime/eviction_sequence.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_confirmations.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheWriteCalls::bind_capacity_calls(const patch::HiCacheSourceDagIndex & source) {
    const auto & graph = source.graph();
    std::map<std::pair<std::string, std::string>, std::map<size_t, size_t>> samples;
    const auto lanes = [&](const HiCacheHostExpansion & plan, const std::string & pid, const std::string & tid) {
        std::vector<size_t> result;
        const auto add = [&](size_t node) {
            const auto lane = graph.node(node).lane_id;
            result.push_back(lane);
            samples[{ pid, tid }].emplace(lane, node);
        };
        for (const auto node : plan.resource_nodes()) add(node);
        return result;
    };
    for (const auto & [key, resources] : write_resources_) {
        for (const auto node : { resources.compute, resources.transfer }) samples[key].emplace(graph.node(node).lane_id, node);
    }
    for (auto & donor : eviction_controls_) donor.position_lanes = lanes(donor.expansion, donor.pid, donor.tid);
    for (const auto & guard : capacity_guards_) {
        const bool existing = std::ranges::any_of(source.fact_nodes(), [&](const auto & fact) {
            return fact.fact_role == "capacity_result_observed" && fact.phase == "end" && fact.pid == guard.pid && fact.tid == guard.tid
                   && guard.begin <= fact.timestamp_us && fact.timestamp_us + fact.duration_us <= guard.end;
        });
        if (existing) continue;
        Call call{ .owner = guard.owner, .pid = guard.pid, .tid = guard.tid, .at_us = guard.end, .entry_node = guard.return_node };
        call.lane = std::string(graph.node_lane_key(guard.return_node));
        for (const auto id : source.outgoing_edge_ids(guard.return_node)) {
            const auto & edge = graph.edge(id);
            if (edge.active && graph.node(edge.dst).active) call.successors.push_back(edge.dst);
        }
        for (const auto & [lane, sample] : samples[{ guard.pid, guard.tid }]) {
            try {
                call.positions.emplace(lane, observe_write_stream_position(source, sample, guard.pid, guard.tid, guard.end));
            }
            catch (const std::runtime_error & error) {
                // Many source guards never need this lane. Retain the reason
                // and require proof only if target work actually uses it.
                capacity_position_errors_[guard.owner].emplace(lane, error.what());
            }
        }
        capacity_at_.emplace(guard.return_node, guard.owner);
        capacity_calls_.emplace(guard.owner, std::move(call));
    }
}

std::optional<size_t> HiCacheWriteCalls::expand_capacity(Call & call, const HiCacheFact & fact, simulation::FutureDag & future,
                                                         std::optional<size_t> previous) {
    const auto * work = replay_.state().eviction_work(fact);
    if (!work) return std::nullopt;
    if (!work->rejected_candidates.empty()) {
        std::string reasons;
        for (const auto & rejected : work->rejected_candidates) {
            if (rejected.reason == "locked_node") continue;
            reasons += " [" + rejected.reason + " node=" + std::to_string(rejected.node) + " before_victim=" + std::to_string(rejected.before_victim) + "]";
        }
        if (!reasons.empty())
            throw std::runtime_error("Capacity branch lacks rejected-candidate control work: request=" + fact.request_id + " pid=" + fact.pid
                                     + " fact=" + std::to_string(fact.source_node_id) + reasons);
    }
    using Kind = HiCacheEvictionRegion::Kind;
    const auto queued = replay_.state().unacknowledged_device_writes(fact);
    const auto predict_control = [&](std::string operation, std::string implementation, std::string timing_scope) {
        const structure::OperationWork work{
            .component = "hicache_io_control",
            .operation = std::move(operation),
            .implementation = std::move(implementation),
            .timing_scope = std::move(timing_scope),
        };
        return operator_provider_.estimate(work).duration_us;
    };
    EvictionControl calibrated{};
    std::vector<size_t> acknowledgement_slots;
    auto release_buffer_pages = work->release_buffer_pages;
    const auto select_host = [&](const HiCacheEvictionStep & step) -> const EvictionControl & {
        if (step.kind == Kind::Control) {
            calibrated.expansion = generate_cpu_control(call.lane, "target eviction control");
            const auto implementation = eviction_control_name(step.control_phase);
            const auto cpu_us = predict_control("eviction.control", implementation, "cpu_service");
            // Residual intervals have separate observations and models. They
            // are a measured approximation, not additional CPU execution.
            const auto residual_us = predict_control("eviction.control", implementation, "residual_interval");
            auto & remainder = control_cost_remainders_[{ call.pid, call.tid }];
            bind_cpu_control(calibrated.expansion, cpu_us, residual_us, remainder);
            calibrated.position_lanes.clear();
            ++generated_control_steps_;
            return calibrated;
        }
        if (step.kind == Kind::BlockingCheck) {
            std::vector<structure::hicache::WriteConfirmationBatch> batches;
            for (const auto & write : queued) {
                // Publication work follows target radix splits, not the size
                // or template of an observed source acknowledgement.
                const auto tokens = core::checked_multiply_u64(write.header.pages.size(), page_size_, "Write acknowledgement token count overflow");
                batches.push_back({ tokens, write.published_nodes });
            }
            const auto program = structure::hicache::write_confirmation_program(batches, true);
            const auto & resources = write_resources_.at({ call.pid, call.tid });
            auto generated = generate_write_confirmation(*graph_, call.entry_node, resources.worker, program);
            bind_write_confirmation(generated, program, operator_provider_);
            calibrated.expansion = std::move(generated.plan);
            acknowledgement_slots = std::move(generated.acknowledgements);
            calibrated.position_lanes.clear();
            ++generated_blocking_checks_;
            return calibrated;
        }
        if (step.kind == Kind::SkipLocked) {
            calibrated.expansion = generate_cpu_control(call.lane, "target locked eviction candidate");
            const structure::OperationWork primitive{
                "hicache_io_control",
                "eviction.locked_candidate",
                "lru_heap",
                "thread_cpu_service",
                { { "pop_count", 1.0 }, { "sum_log2_heap", std::log2(static_cast<double>(step.heap_size)) } }
            };
            const auto cpu_us = operator_provider_.estimate(primitive).duration_us;
            auto & remainder = control_cost_remainders_[{ call.pid, call.tid }];
            // The isolated primitive measures thread CPU only. It is not a
            // calibration of scheduler delay or an entire eviction envelope.
            bind_cpu_control(calibrated.expansion, cpu_us, 0, remainder);
            calibrated.position_lanes.clear();
            return calibrated;
        }
        if (step.kind == Kind::ReleaseBackup || step.kind == Kind::ReleaseRegular) {
            const auto pages = work->victims.at(*step.victim).page_count;
            const auto & resources = write_resources_.at({ call.pid, call.tid });
            auto generated = generate_paged_release(*graph_, call.entry_node, resources.worker, resources.compute, page_size_ > 1);
            const auto costs = cost::estimate_paged_release(operator_provider_,
                                                            pages,
                                                            page_size_,
                                                            release_buffer_pages,
                                                            step.kind == Kind::ReleaseBackup,
                                                            allocator_need_sort_);
            bind_paged_release(generated, costs);
            calibrated.expansion = std::move(generated.plan);
            calibrated.position_lanes = calibrated.expansion.resource_lanes(*graph_);
            release_buffer_pages = core::checked_add_u64(release_buffer_pages, pages, "Target release buffer overflow");
            return calibrated;
        }
        throw std::logic_error("Unsupported target eviction step");
    };
    const auto check_positions = [&](std::span<const size_t> lanes) {
        for (const auto lane : lanes) {
            if (call.positions.contains(lane)) continue;
            // A newly required resource may have no source observation at all.
            // Do not mask that missing dependency with an exception while
            // looking up an optional diagnostic from source preparation.
            std::string reason = "no source position observation for target resource";
            if (const auto owner = capacity_position_errors_.find(call.owner); owner != capacity_position_errors_.end())
                if (const auto issue = owner->second.find(lane); issue != owner->second.end()) reason = issue->second;
            throw std::runtime_error("Target capacity stream position is unavailable: owner=" + std::to_string(call.owner) + " lane=" + std::to_string(lane)
                                     + " pid=" + call.pid + " tid=" + call.tid + " reason=" + reason);
        }
    };
    HiCacheHostSequence sequence(call.positions, stream_insertions_, future, previous);
    std::optional<size_t> last;
    if (const auto failure = load_failure_templates_.find(call.owner); failure != load_failure_templates_.end())
        last = sequence.append(failure->second, std::span<const size_t>{}).host_return;
    for (const auto & step : eviction_sequence(*work, write_back_)) {
        if (step.kind == Kind::WriteBackup) {
            const auto victim = work->victims.at(*step.victim).node;
            const auto operation =
                std::ranges::find_if(queued, [&](const auto & write) { return write.node == victim && write.header.source_node_id == call.owner; });
            if (operation == queued.end()) throw std::runtime_error("Selected target victim has no queued write");
            const auto & donor = write_resources_.at({ call.pid, call.tid });
            check_positions(donor.position_lanes);
            const auto expanded = submit_target_write(donor, fact, *operation, sequence, future);
            last = expanded.host_return;
        }
        else {
            const auto & donor = select_host(step);
            check_positions(donor.position_lanes);
            std::vector<size_t> events;
            if (step.kind == Kind::BlockingCheck)
                for (const auto & write : queued) {
                    const auto completion = completions_.find({ write.header.cache_scope, write.header.operation_id });
                    if (completion == completions_.end()) throw std::runtime_error("Capacity check refers to an unsubmitted target write");
                    events.push_back(completion->second);
                }
            const auto expanded = sequence.append(donor.expansion, donor.position_lanes, events);
            last = expanded.host_return;
            if (step.kind == Kind::BlockingCheck)
                for (const auto slot : acknowledgement_slots) capacity_ack_.emplace(expanded.nodes.at(slot), call.owner);
        }
    }
    if (!last) throw std::logic_error("Capacity sequence has no return");
    for (const auto successor : call.successors) future.depend(*last, successor);
    if (!call.retained_allocation_return) {
        allocation_returns_.erase(call.entry_node);
        allocation_returns_[*last].push_back(call.owner);
    }
    return last;
}

} // namespace markov::trace_graph::modules::hicache::runtime
