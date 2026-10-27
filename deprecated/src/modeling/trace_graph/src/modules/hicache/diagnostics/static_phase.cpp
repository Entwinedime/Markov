#include "markov/trace_graph/modules/hicache/diagnostics/static_phase.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/attention_program.hpp"

#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include "markov/trace_graph/structure/phase_shape.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::modules::hicache::diagnostics {

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

} // namespace

StaticPhaseCosts observed_static_phase_costs(const core::DagGraph & graph, const modules::hicache::model::HiCachePhaseWorkLedger & work) {
    StaticPhaseCosts costs;
    costs.status = work.status;
    costs.cost_status = work.status == "ready" ? "ready" : "not_ready";
    const auto observed = [&](const std::vector<size_t> & nodes) {
        uint64_t duration = 0;
        for (const auto node : nodes) duration = core::checked_add_u64(duration, graph.node(node).duration, "Static phase source cost overflow");
        return StaticPhaseNodeCost{ duration, duration, nodes };
    };
    for (const auto & work_item : work.prefills) {
        StaticPrefillCost item;
        static_cast<modules::hicache::model::HiCachePrefillWorkItem &>(item) = work_item;
        item.common_kernel_cost = observed(work_item.common_kernel_nodes);
        item.prefix_attention_cost = observed(work_item.prefix_attention_nodes);
        item.collective_cost = observed(work_item.collective_nodes);
        item.submit_cost = observed(work_item.submit_nodes);
        item.feature_covered = true;
        costs.prefills.push_back(std::move(item));
    }
    for (const auto & work_item : work.decodes) {
        StaticDecodeCost item;
        static_cast<modules::hicache::model::HiCacheDecodeWorkItem &>(item) = work_item;
        item.kernel_cost = observed(work_item.kernel_nodes);
        item.collective_cost = observed(work_item.collective_nodes);
        item.submit_cost = observed(work_item.submit_nodes);
        for (const auto node : work_item.kernel_nodes)
            if (adapters::sglang::is_attention_kernel(graph.event_for_node(node).name))
                item.source_paged_attention_duration_us =
                    core::checked_add_u64(item.source_paged_attention_duration_us, graph.node(node).duration, "Static attention source cost overflow");
        item.predicted_paged_attention_duration_us = item.source_paged_attention_duration_us;
        item.feature_covered = true;
        costs.decodes.push_back(std::move(item));
    }
    return costs;
}

StaticPhaseCosts bind_static_phase_costs(const core::DagGraph & graph, const modules::hicache::model::HiCachePhaseWorkLedger & structure,
                                         const frontend::HiCachePhaseCostConfig & config) {
    auto work = observed_static_phase_costs(graph, structure);
    if (!config.enabled) {
        work.cost_status = "disabled";
        return work;
    }

    for (auto & prefill : work.prefills) {
        const auto attention_work_pairs =
            structure::PrefillShape{ prefill.prefill_token_count, prefill.reusable_prefix_token_count, prefill.batch_size, prefill.target_page_size }
                .attention_work_pairs();
        prefill.feature_covered = feature_covered(config, prefill.prefill_token_count, prefill.reusable_prefix_token_count, attention_work_pairs);
        prefill.common_kernel_cost.predicted_duration_us = token_curve_duration(config.prefill_common_kernel, prefill.prefill_token_count);
        prefill.prefix_attention_cost.predicted_duration_us =
            phase_duration(config.prefill_prefix_attention, prefill.prefill_token_count, prefill.reusable_prefix_token_count, attention_work_pairs);
        prefill.collective_cost.predicted_duration_us = token_curve_duration(config.prefill_collective, prefill.prefill_token_count);
        prefill.submit_cost.predicted_duration_us = prefill.submit_cost.source_duration_us;
    }

    for (auto & decode : work.decodes) {
        if (decode.iteration_count == 0) continue;
        if (!config.decode_paged_attention || !config.decode_collective) throw std::runtime_error("Static Decode replay requires explicit aggregate costs");

        const auto [per_iteration, pages] = decode_paged_attention_duration(*config.decode_paged_attention, decode.prompt_token_count, decode.target_page_size);
        decode.effective_page_count = pages;
        decode.feature_covered = decode.prompt_token_count >= config.min_decode_context_tokens && decode.prompt_token_count <= config.max_decode_context_tokens;
        decode.predicted_paged_attention_duration_us = core::checked_multiply_u64(per_iteration, decode.iteration_count, "Static Decode cost overflow");
        if (decode.source_paged_attention_duration_us > decode.kernel_cost.source_duration_us)
            throw std::runtime_error("Static Decode attention exceeds its source kernel ownership");
        decode.kernel_cost.predicted_duration_us = core::checked_add_u64(decode.kernel_cost.source_duration_us - decode.source_paged_attention_duration_us,
                                                                         decode.predicted_paged_attention_duration_us,
                                                                         "Static Decode kernel cost overflow");
        decode.collective_cost.predicted_duration_us = core::checked_multiply_u64(phase_duration(*config.decode_collective, 0, decode.prompt_token_count, 0.0),
                                                                                  decode.iteration_count,
                                                                                  "Static Decode collective cost overflow");
        decode.submit_cost.predicted_duration_us = decode.submit_cost.source_duration_us;
    }
    work.cost_status = "ready";
    return work;
}

} // namespace markov::trace_graph::modules::hicache::diagnostics
