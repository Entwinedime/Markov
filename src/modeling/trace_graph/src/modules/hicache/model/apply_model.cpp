/**
 * @file
 * @brief HiCache fact scanning, target-state replay, and Debug summary convergence.
 */
#include "markov/trace_graph/modules/hicache/model/replay.hpp"

#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/missing_cost.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::model {

using core::DagGraph;
using frontend::HiCacheConfig;

namespace {

uint64_t phase_duration(const frontend::HiCachePhaseLinearCostConfig & cost, uint64_t new_tokens, uint64_t context_tokens, double attention_token_pairs) {
    const auto value = cost.fixed_us + cost.per_new_token_us * static_cast<double>(new_tokens) + cost.per_attention_token_pair_us * attention_token_pairs
                       + cost.per_context_token_us * static_cast<double>(context_tokens);
    const auto rounded = core::truncate_to_u64(value + 0.5);
    if (!rounded) throw std::overflow_error("HiCache phase cost exceeds uint64 range");
    return *rounded;
}

uint64_t token_curve_duration(const frontend::HiCachePhaseTokenCostConfig & cost, uint64_t new_tokens) {
    if (cost.points.size() < 2) throw std::logic_error("HiCache phase token curve requires two anchors");
    auto right = std::ranges::lower_bound(cost.points, new_tokens, {}, &frontend::HiCachePhaseTokenCostPoint::new_tokens);
    if (right == cost.points.begin()) ++right;
    else if (right == cost.points.end()) right = std::prev(cost.points.end());
    const auto left = std::prev(right);
    const auto position = (static_cast<double>(new_tokens) - static_cast<double>(left->new_tokens)) / static_cast<double>(right->new_tokens - left->new_tokens);
    const auto value = std::max(0.0, left->duration_us + position * (right->duration_us - left->duration_us));
    const auto rounded = core::truncate_to_u64(value + 0.5);
    if (!rounded) throw std::overflow_error("HiCache phase token-curve cost exceeds uint64 range");
    return *rounded;
}

std::pair<uint64_t, uint64_t> decode_paged_attention_duration(const frontend::HiCacheDecodePagedAttentionCostConfig & cost, uint64_t context_tokens,
                                                              uint64_t target_page_size) {
    if (cost.kernel_page_tokens == 0 || target_page_size == 0) throw std::logic_error("HiCache Decode paged-attention page geometry is incomplete");
    const auto effective_page_tokens = std::min(target_page_size, cost.kernel_page_tokens);
    const auto effective_pages = context_tokens / effective_page_tokens + static_cast<uint64_t>(context_tokens % effective_page_tokens != 0);
    const auto value = cost.fixed_us_per_iteration + cost.per_context_token_us * static_cast<double>(context_tokens)
                       + cost.per_effective_page_us * static_cast<double>(effective_pages);
    const auto rounded = core::truncate_to_u64(value + 0.5);
    if (!rounded) throw std::overflow_error("HiCache Decode paged-attention cost exceeds uint64 range");
    return { *rounded, effective_pages };
}

bool feature_covered(const frontend::HiCachePhaseCostConfig & cost, uint64_t new_tokens, uint64_t context_tokens, double attention_token_pairs) {
    const auto token_covered = new_tokens >= cost.min_new_tokens && new_tokens <= cost.max_new_tokens;
    const auto prefix_covered = context_tokens >= cost.min_context_tokens && context_tokens <= cost.max_context_tokens
                                && attention_token_pairs >= cost.min_attention_token_pairs && attention_token_pairs <= cost.max_attention_token_pairs;
    return token_covered && prefix_covered;
}

HiCachePhaseNodeCostPlan node_cost(uint64_t source_duration_us, uint64_t predicted_duration_us, const std::vector<size_t> & source_nodes) {
    return HiCachePhaseNodeCostPlan{
        .source_duration_us = source_duration_us,
        .predicted_duration_us = predicted_duration_us,
        .source_node_ids = source_nodes,
    };
}

} // namespace

HiCacheModelResult apply_hicache_model(DagGraph & graph, const HiCacheConfig & config, const HiCacheFactClock & execution_clock) {
    HiCacheModelReplay replay(graph, config, false, execution_clock);
    for (const auto & item : replay.facts()) replay.apply(item.fact.source_node_id, item.fact.ts);
    return replay.finish();
}

HiCacheModelResult HiCacheModelReplay::finish() {
    if (finished_) throw std::logic_error("HiCache replay already finished");
    if (std::ranges::any_of(facts_, [](const auto & item) { return !item.consumed; }))
        throw std::logic_error("HiCache replay cannot finish before all formal facts execute");

    finished_ = true;
    if (result_.effect_decisions.status == "needs_calibration") return result_;
    // Live execution has explicit completion/release events. Reporting must not
    // manufacture an acknowledgement or cancel in-flight work at end of trace.
    if (!execution_prefetch_control_) state_.finalize();

    (void)current_phase_work();
    auto result = result_;
    auto effect_decisions = state_.effect_decision_ledger();
    for (const auto & [reason, count] : result.effect_decisions.missing_facts) {
        auto & merged_count = effect_decisions.missing_facts[reason];
        merged_count = core::checked_add_u64(merged_count, count, "HiCache merged missing-fact count exceeds uint64 range");
    }
    if (!effect_decisions.missing_facts.empty()) effect_decisions.status = "partial";

    result.effect_decisions = std::move(effect_decisions);
    result.replay_complete = true;
    return result;
}

const HiCachePhaseWorkLedger & HiCacheModelReplay::current_phase_work() {
    auto & work = result_.phase_work;
    if (result_.effect_decisions.status == "needs_calibration") return work;

    const auto & phase_cost = result_.phase_cost_model;
    const auto & prefills = state_.prefill_work_items();
    const auto & allocations = state_.allocator_work_items();
    const auto admitted = work.prefills.size();
    // State appends immutable admissions only after allocation succeeds, also
    // when a blocked allocation resumes. Earlier costs cannot change afterward.
    work.prefills.insert(work.prefills.end(), prefills.begin() + admitted, prefills.end());
    work.allocator_calls.insert(work.allocator_calls.end(), allocations.begin() + work.allocator_calls.size(), allocations.end());

    for (size_t i = admitted; i < work.prefills.size(); ++i) {
        auto & prefill = work.prefills[i];
        const auto found = source_phase_positions_.find({ prefill.pid, prefill.request_id });
        if (found == source_phase_positions_.end()) {
            (void)core::checked_increment_u64(work.blockers["source_phase_request_missing"], "HiCache phase blocker count exceeds uint64 range");
            continue;
        }

        const auto & source = source_phases_.observations[found->second];
        prefill.logical_input = source.logical_input;
        prefill.source_prefill_token_count = source.prefill_token_count;
        const auto context_tokens = prefill.reusable_prefix_token_count;
        prefill.attention_token_pairs =
            static_cast<double>(prefill.prefill_token_count) * (static_cast<double>(context_tokens) + static_cast<double>(prefill.prefill_token_count) / 2.0);
        if (phase_cost.enabled) {
            prefill.feature_covered = feature_covered(phase_cost, prefill.prefill_token_count, context_tokens, prefill.attention_token_pairs);
            const auto common_duration = token_curve_duration(phase_cost.prefill_common_kernel, prefill.prefill_token_count);
            const auto prefix_duration =
                phase_duration(phase_cost.prefill_prefix_attention, prefill.prefill_token_count, context_tokens, prefill.attention_token_pairs);
            prefill.common_kernel_cost = node_cost(source.prefill_common_kernel_duration_us, common_duration, source.prefill_common_kernel_node_ids);
            prefill.prefix_attention_cost = node_cost(source.prefill_prefix_attention_duration_us, prefix_duration, source.prefill_prefix_attention_node_ids);
            (void)core::checked_add_u64(common_duration, prefix_duration, "HiCache prefill kernel duration exceeds uint64 range");
            prefill.collective_cost = node_cost(source.prefill_collective_duration_us,
                                                token_curve_duration(phase_cost.prefill_collective, prefill.prefill_token_count),
                                                source.prefill_collective_node_ids);
            prefill.submit_cost = node_cost(source.prefill_submit_cpu_duration_us, source.prefill_submit_cpu_duration_us, source.prefill_submit_cpu_node_ids);
        }

        HiCacheDecodeWorkItem decode{
            .logical_input = source.logical_input,
            .pid = source.pid,
            .request_id = prefill.request_id,
            .prompt_token_count = prefill.prompt_token_count,
            .target_page_size = prefill.target_page_size,
            .iteration_count = source.decode_iteration_count,
        };
        if (phase_cost.enabled && decode.iteration_count > 0) {
            if (!phase_cost.decode_paged_attention || !phase_cost.decode_collective)
                throw MissingCostEvidence("phase/decode",
                                          {
                                              { "iteration_count", decode.iteration_count }
                },
                                          "Executed Decode requires measured phase costs");
            decode.feature_covered =
                prefill.prompt_token_count >= phase_cost.min_decode_context_tokens && prefill.prompt_token_count <= phase_cost.max_decode_context_tokens;
            decode.source_paged_attention_duration_us = hicache_paged_attention_duration(source.decode_kernel_families);
            if (decode.source_paged_attention_duration_us == 0 || decode.source_paged_attention_duration_us > source.decode_kernel_duration_us) {
                (void)core::checked_increment_u64(work.blockers["source_decode_paged_attention_missing"], "HiCache phase blocker count exceeds uint64 range");
            }
            else {
                const auto [paged_per_iteration, effective_pages] =
                    decode_paged_attention_duration(*phase_cost.decode_paged_attention, decode.prompt_token_count, decode.target_page_size);
                decode.effective_page_count = effective_pages;
                decode.predicted_paged_attention_duration_us =
                    core::checked_multiply_u64(paged_per_iteration, decode.iteration_count, "HiCache Decode paged-attention duration exceeds uint64 range");
                const auto source_non_paged = source.decode_kernel_duration_us - decode.source_paged_attention_duration_us;
                decode.kernel_cost = node_cost(source.decode_kernel_duration_us,
                                               core::checked_add_u64(source_non_paged,
                                                                     decode.predicted_paged_attention_duration_us,
                                                                     "HiCache Decode kernel duration exceeds uint64 range"),
                                               source.decode_kernel_node_ids);
            }
            decode.collective_cost = node_cost(source.decode_collective_duration_us,
                                               core::checked_multiply_u64(phase_duration(*phase_cost.decode_collective, 0, decode.prompt_token_count, 0.0),
                                                                          decode.iteration_count,
                                                                          "HiCache decode collective duration exceeds uint64 range"),
                                               source.decode_collective_node_ids);
            decode.submit_cost = node_cost(source.decode_submit_cpu_duration_us, source.decode_submit_cpu_duration_us, source.decode_submit_cpu_node_ids);
        }
        work.decodes.push_back(std::move(decode));
    }

    // This aggregate blocker is derived below, not accumulated per observation.
    work.blockers.erase("source_phase_cost_nodes_missing");
    work.prefill_status = source_phases_.ready() && work.prefills.size() == source_phases_.observations.size() && work.blockers.empty() ? "ready" : "not_ready";
    work.decode_status = work.decodes.size() == work.prefills.size() && work.blockers.empty() ? "ready" : "not_ready";
    if (!phase_cost.enabled) work.cost_status = "disabled";
    else {
        const auto prefill_cost_ready = std::ranges::all_of(work.prefills, [](const auto & item) {
            return !item.common_kernel_cost.source_node_ids.empty() && !item.collective_cost.source_node_ids.empty()
                   && !item.submit_cost.source_node_ids.empty();
        });
        const auto decode_cost_ready = std::ranges::all_of(work.decodes, [](const auto & item) {
            return !item.kernel_cost.source_node_ids.empty() && !item.collective_cost.source_node_ids.empty() && !item.submit_cost.source_node_ids.empty();
        });
        work.cost_status = prefill_cost_ready && decode_cost_ready ? "ready" : "not_ready";
        if (!prefill_cost_ready || !decode_cost_ready)
            (void)core::checked_increment_u64(work.blockers["source_phase_cost_nodes_missing"], "HiCache phase blocker count exceeds uint64 range");
    }
    work.status = work.prefill_status == "ready" && work.decode_status == "ready" ? "ready" : "not_ready";
    return work;
}

} // namespace markov::trace_graph::modules::hicache::model
