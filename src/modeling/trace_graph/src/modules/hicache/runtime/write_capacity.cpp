#include "markov/trace_graph/modules/hicache/missing_cost.hpp"
#include "markov/trace_graph/modules/hicache/runtime/eviction_sequence.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include <algorithm>
#include <stdexcept>

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
    for (const auto & [id, donor] : templates_) (void)lanes(donor.expansion, donor.pid, donor.tid);
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
            if (rejected.reason == "locked_node" && locked_candidate_us_) continue;
            if (rejected.reason == "locked_node")
                throw MissingCostEvidence("execution_control/eviction_locked_candidate",
                                          {
                                              { "heap_size", rejected.heap_size }
                },
                                          "Target eviction needs heap-pop and locked-candidate check CPU cost");
            reasons += " [" + rejected.reason + " node=" + std::to_string(rejected.node) + " before_victim=" + std::to_string(rejected.before_victim) + "]";
        }
        if (!reasons.empty())
            throw std::runtime_error("Capacity branch lacks rejected-candidate control work: request=" + fact.request_id + " pid=" + fact.pid
                                     + " fact=" + std::to_string(fact.source_node_id) + reasons);
    }
    using Kind = HiCacheEvictionRegion::Kind;
    const auto queued = replay_.state().unacknowledged_device_writes(fact);
    const auto control_samples = [&] {
        std::vector<EvictionControlCostSample> samples;
        for (const auto & donor : eviction_controls_) {
            const auto & plan = donor.expansion;
            if (donor.pid != call.pid || donor.tid != call.tid || donor.region.kind != Kind::Control || !plan.streams.empty() || !plan.waits.empty()
                || !plan.event_waits.empty() || std::ranges::any_of(plan.nodes, [&](const auto & node) {
                       return !node.work.is_cpu || node.submission.has_value() || node.work.lane_key != call.lane;
                   }))
                continue;
            EvictionControlCostSample sample{ donor.region.control_phase };
            sample.independent = donor.independent;
            for (const auto & node : plan.nodes) {
                sample.cpu_us += node.work.duration;
                sample.residual_us += node.work.cpu_gap_after;
            }
            samples.push_back(sample);
        }
        return samples;
    }();
    EvictionControl calibrated{};
    const auto select_host = [&](const HiCacheEvictionStep & step) -> const EvictionControl & {
        if (step.kind == Kind::Control) {
            const auto cost = estimate_eviction_control_cost(control_samples, step.control_phase);
            if (!cost)
                throw MissingCostEvidence("execution_control/eviction_control",
                                          {
                                              { "phase", static_cast<int>(step.control_phase) }
                },
                                          "Target eviction control has no base or independent CPU cost evidence");
            auto & remainder = control_cost_remainders_[{ call.pid, call.tid }];
            calibrated.expansion = generated_cpu_control(call.lane,
                                                         cost->cpu_us,
                                                         cost->residual_us,
                                                         remainder,
                                                         cost->independent          ? "target eviction control: shared estimate"
                                                         : cost->phase_extrapolated ? "target eviction control: pooled base estimate"
                                                                                    : "target eviction control: base phase estimate");
            calibrated.position_lanes.clear();
            ++generated_control_steps_;
            phase_extrapolated_control_steps_ += cost->phase_extrapolated;
            return calibrated;
        }
        if (step.kind == Kind::BlockingCheck) {
            const auto proxy = estimate_eviction_control_cost(control_samples, HiCacheEvictionRegion::ControlPhase::None);
            if ((!queued.empty() && !proxy) || (!empty_write_check_us_ && !proxy))
                throw MissingCostEvidence("execution_control/write_completion_check",
                                          {
                                              { "queued_writes", queued.size() }
                },
                                          "Target blocking check has no CPU cost evidence");
            const auto entry = empty_write_check_us_ ? *empty_write_check_us_ : proxy->cpu_us;
            auto & remainder = control_cost_remainders_[{ call.pid, call.tid }];
            calibrated.expansion = generated_completion_check(call.lane,
                                                              queued.size(),
                                                              entry,
                                                              empty_write_check_us_ ? 0 : proxy->residual_us,
                                                              proxy ? proxy->cpu_us : 0,
                                                              proxy ? proxy->residual_us : 0,
                                                              remainder);
            calibrated.position_lanes.clear();
            ++generated_blocking_checks_;
            blocking_check_cost_extrapolations_ += !queued.empty() || !empty_write_check_us_;
            if (queued.empty() && empty_write_check_us_) {
                ++calibrated_empty_checks_;
                calibrated_empty_check_cpu_us_ += calibrated.expansion.nodes.front().work.duration;
            }
            return calibrated;
        }
        if (step.kind == Kind::SkipLocked) {
            auto & remainder = locked_candidate_remainder_us_[{ call.pid, call.tid }];
            const auto cost = locked_candidate_cost(step.heap_size, *locked_candidate_us_, locked_candidate_log2_heap_us_);
            calibrated.expansion = calibrated_cpu_control(call.lane, cost, remainder, "locked eviction candidate");
            return calibrated;
        }
        std::vector<const EvictionControl *> candidates;
        uint64_t nearest = 0;
        const bool release = step.kind == Kind::ReleaseBackup || step.kind == Kind::ReleaseRegular;
        const auto tokens = release ? work->victims.at(*step.victim).page_count * page_size_ : 0;
        for (const auto & donor : eviction_controls_) {
            if (donor.pid != call.pid || donor.tid != call.tid || donor.region.kind != step.kind || donor.region.control_phase != step.control_phase) continue;
            if (!donor.expansion.event_waits.empty()) continue;
            if (release && !donor.region.released_tokens) continue;
            const auto observed = donor.region.released_tokens;
            const auto distance = release ? (tokens > observed ? tokens - observed : observed - tokens) : 0;
            if (!candidates.empty() && !candidates.front()->independent && donor.independent) continue;
            if (candidates.empty() || (candidates.front()->independent && !donor.independent) || distance < nearest) {
                candidates.clear();
                nearest = distance;
            }
            if (distance == nearest) candidates.push_back(&donor);
        }
        if (candidates.empty() && release)
            throw MissingCostEvidence(step.kind == Kind::ReleaseRegular ? "execution_control/release_regular" : "execution_control/release_backup",
                                      {
                                          { "released_tokens",     tokens },
                                          {       "page_size", page_size_ }
            },
                                      "Target release needs same-operation CPU/device work with allocator dependencies, from base or shared calibration");
        if (candidates.empty())
            throw std::runtime_error("Target capacity step lacks a same-thread base template: kind=" + std::to_string(static_cast<int>(step.kind))
                                     + " phase=" + std::to_string(static_cast<int>(step.control_phase)) + " tokens=" + std::to_string(tokens)
                                     + " queued_writes=" + std::to_string(queued.size())
                                     + " preceding_kind=" + (step.preceding_kind ? std::to_string(static_cast<int>(*step.preceding_kind)) : "none"));
        const auto service = [](const EvictionControl * donor) {
            uint64_t total = 0;
            for (const auto & node : donor->expansion.nodes)
                if (node.work.is_cpu) total += node.work.duration + node.work.cpu_gap_after;
            return total;
        };
        std::ranges::stable_sort(candidates, {}, service);
        return *candidates[candidates.size() / 2];
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
            const auto & donor = select_write_template(call.pid, call.tid, operation->schedule.effective_byte_count);
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
            if (step.kind == Kind::BlockingCheck) capacity_ack_.emplace(*last, call.owner);
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
