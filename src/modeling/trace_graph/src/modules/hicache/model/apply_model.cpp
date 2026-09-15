/**
 * @file
 * @brief HiCache fact scanning, target-state replay, and Debug summary convergence.
 */
#include "markov/trace_graph/modules/hicache/model/state.hpp"

#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>

namespace markov::trace_graph::modules::hicache::model {

using core::DagGraph;
using frontend::HiCacheConfig;

namespace {

struct SourceGapSlice {
    uint64_t start_us = 0;
    uint64_t source_duration_us = 0;
    uint64_t residual_duration_us = 0;
};

using SourceLane = std::pair<int, std::string>;

std::optional<int> rank_from_cache_scope(std::string_view scope) {
    constexpr std::string_view prefix = "rank:";
    if (!scope.starts_with(prefix)) return std::nullopt;
    scope.remove_prefix(prefix.size());
    const auto end = scope.find(':');
    const auto value = scope.substr(0, end);
    int rank = 0;
    const auto [position, error] = std::from_chars(value.data(), value.data() + value.size(), rank);
    if (error != std::errc{} || position != value.data() + value.size() || rank < 0) return std::nullopt;
    return rank;
}

std::map<SourceLane, std::vector<SourceGapSlice>> source_residual_gaps(const core::DagGraph & graph) {
    std::map<SourceLane, std::vector<SourceGapSlice>> gaps;
    for (const auto & node : graph.nodes()) {
        if (!node.active || !node.is_cpu || node.kind != core::DagNodeKind::TraceEvent || node.original_cpu_gap_after == 0) continue;
        const auto & event = graph.event_for_node(node.id);
        const auto owned = std::min(node.original_cpu_gap_after, graph.scope_gap_duration(node.id));
        const auto residual = node.original_cpu_gap_after - owned;
        if (residual == 0) continue;
        gaps[{node.gpu_id, event.tid}].push_back(SourceGapSlice{
            .start_us = core::checked_add_u64(event.ts, event.dur, "source CPU gap boundary overflow"),
            .source_duration_us = node.original_cpu_gap_after,
            .residual_duration_us = residual,
        });
    }
    for (auto & slices : gaps | std::views::values) std::ranges::sort(slices, {}, &SourceGapSlice::start_us);
    return gaps;
}

uint64_t target_fact_timestamp(const std::map<SourceLane, std::vector<SourceGapSlice>> & gaps, const HiCacheFact & fact) {
    const auto rank = rank_from_cache_scope(fact.cache_scope);
    if (!rank) return fact.source_ts;
    const auto found = gaps.find({*rank, fact.tid});
    if (found == gaps.end()) return fact.source_ts;
    uint64_t removed = 0;
    for (const auto & gap : found->second) {
        if (fact.source_ts <= gap.start_us) break;
        const auto elapsed = std::min(fact.source_ts - gap.start_us, gap.source_duration_us);
        const auto proportional = static_cast<uint64_t>(static_cast<long double>(elapsed)
                                                        * static_cast<long double>(gap.residual_duration_us)
                                                        / static_cast<long double>(gap.source_duration_us));
        removed = core::checked_add_u64(removed, proportional, "source residual-gap projection overflow");
        if (elapsed < gap.source_duration_us) break;
    }
    return removed < fact.source_ts ? fact.source_ts - removed : 0;
}

uint64_t phase_duration(const frontend::HiCachePhaseLinearCostConfig & cost, uint64_t new_tokens, uint64_t context_tokens,
                        double attention_token_pairs) {
    const auto value = cost.fixed_us + cost.per_new_token_us * static_cast<double>(new_tokens)
                       + cost.per_attention_token_pair_us * attention_token_pairs
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
    const auto position = (static_cast<double>(new_tokens) - static_cast<double>(left->new_tokens))
                          / static_cast<double>(right->new_tokens - left->new_tokens);
    const auto value = std::max(0.0, left->duration_us + position * (right->duration_us - left->duration_us));
    const auto rounded = core::truncate_to_u64(value + 0.5);
    if (!rounded) throw std::overflow_error("HiCache phase token-curve cost exceeds uint64 range");
    return *rounded;
}

std::pair<uint64_t, uint64_t> decode_paged_attention_duration(
    const frontend::HiCacheDecodePagedAttentionCostConfig & cost,
    uint64_t context_tokens,
    uint64_t target_page_size) {
    if (cost.kernel_page_tokens == 0 || target_page_size == 0)
        throw std::logic_error("HiCache Decode paged-attention page geometry is incomplete");
    const auto effective_page_tokens = std::min(target_page_size, cost.kernel_page_tokens);
    const auto effective_pages = context_tokens / effective_page_tokens
                                 + static_cast<uint64_t>(context_tokens % effective_page_tokens != 0);
    const auto value = cost.fixed_us_per_iteration + cost.per_context_token_us * static_cast<double>(context_tokens)
                       + cost.per_effective_page_us * static_cast<double>(effective_pages);
    const auto rounded = core::truncate_to_u64(value + 0.5);
    if (!rounded) throw std::overflow_error("HiCache Decode paged-attention cost exceeds uint64 range");
    return { *rounded, effective_pages };
}

bool feature_covered(const frontend::HiCachePhaseCostConfig & cost, uint64_t new_tokens, uint64_t context_tokens,
                     double attention_token_pairs) {
    const auto token_covered = new_tokens >= cost.min_new_tokens && new_tokens <= cost.max_new_tokens;
    const auto prefix_covered = context_tokens >= cost.min_context_tokens
                                && context_tokens <= cost.max_context_tokens
                                && attention_token_pairs >= cost.min_attention_token_pairs
                                && attention_token_pairs <= cost.max_attention_token_pairs;
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

/**
 * @brief Replays approved HiCache facts and emits a Debug module summary.
 *
 * The pass first indexes approved token dictionaries, then dispatches facts in stable DAG
 * node order, finalizes target-derived asynchronous lifecycles, and finally aggregates
 * diagnostics in Debug builds. State replay never consumes source-actual outcomes.
 */
HiCacheModelResult apply_hicache_model(DagGraph & graph, const HiCacheConfig & config) {
    struct OrderedHiCacheFact {
        const core::TraceEvent * event = nullptr;
        size_t source_fact_id = 0;
        std::optional<size_t> execution_anchor_node_id = std::nullopt;
        uint64_t boundary_ts = 0;
        bool prelude = false;
        bool causal_tail = false;
    };
    struct RoutedHiCacheFact {
        HiCacheFact fact;
        HiCacheFactRoute route;
        std::vector<std::string> required_errors;
        bool prelude = false;
        bool causal_tail = false;
    };

    HiCacheModelResult result;
    result.io_cost_model = config.io_cost;
    result.phase_cost_model = config.phase_cost;
    // State readiness and DAG durations use the same calibrated service model.
    if (config.kv_bytes_per_page == 0) result.effect_decisions.missing_facts["kv_geometry"] = 1;
    if (config.io_cost.service_models.empty() || config.io_cost.storage_batch_pages == 0)
        result.effect_decisions.missing_facts["io_service_model"] = 1;
    if (!result.effect_decisions.missing_facts.empty()) {
        result.effect_decisions.status = "needs_calibration";
        result.phase_work.blockers = result.effect_decisions.missing_facts;
        return result;
    }
    HiCacheState state(config);
    const auto residual_gaps = source_residual_gaps(graph);

    HiCacheFactParser parser;
    for (const auto & event : graph.context_events()) parser.observe_token_dictionaries(event);
    std::vector<OrderedHiCacheFact> hicache_facts;
    hicache_facts.reserve(graph.prelude_context_events().size() + graph.hicache_fact_events().size() + graph.tail_context_events().size());
    for (const auto & event : graph.prelude_context_events()) {
        if (!parser.is_hicache_event(event)) continue;
        hicache_facts.push_back(OrderedHiCacheFact{
            .event = &event,
            .source_fact_id = std::numeric_limits<size_t>::max() - event.index,
            .boundary_ts = hicache_fact_boundary_timestamp(event),
            .prelude = true,
        });
        parser.observe_token_dictionaries(event);
    }
    for (const auto & event : graph.hicache_fact_events()) {
        if (!parser.is_hicache_event(event)) continue;
        hicache_facts.push_back(OrderedHiCacheFact{
            .event = &event,
            .source_fact_id = event.index,
            .boundary_ts = hicache_fact_boundary_timestamp(event),
        });
        parser.observe_token_dictionaries(event);
    }
    for (const auto & event : graph.tail_context_events()) {
        if (!parser.is_hicache_event(event)) continue;
        if (event.index > std::numeric_limits<size_t>::max() - graph.node_count())
            throw std::overflow_error("HiCache causal-tail fact identity exceeds size_t range");
        hicache_facts.push_back(OrderedHiCacheFact{
            .event = &event,
            .source_fact_id = graph.node_count() + event.index,
            .boundary_ts = hicache_fact_boundary_timestamp(event),
            .causal_tail = true,
        });
        parser.observe_token_dictionaries(event);
    }
    std::ranges::sort(hicache_facts, [&](const OrderedHiCacheFact & left, const OrderedHiCacheFact & right) {
        const auto & lhs = *left.event;
        const auto & rhs = *right.event;
        if (left.boundary_ts != right.boundary_ts) return left.boundary_ts < right.boundary_ts;
        if (lhs.pid != rhs.pid) return lhs.pid < rhs.pid;
        if (lhs.tid != rhs.tid) return lhs.tid < rhs.tid;
        if (lhs.name != rhs.name) return lhs.name < rhs.name;
        if (lhs.index != rhs.index) return lhs.index < rhs.index;
        return left.source_fact_id < right.source_fact_id;
    });

    std::vector<RoutedHiCacheFact> routed_facts;
    routed_facts.reserve(hicache_facts.size());
    for (const auto & ordered_fact : hicache_facts) {
        const auto & event = *ordered_fact.event;
        auto fact = parser.parse(ordered_fact.source_fact_id, event, ordered_fact.execution_anchor_node_id);
        fact.ts = target_fact_timestamp(residual_gaps, fact);

        auto route = route_hicache_fact(fact);
        auto required_errors = route.model_fact && route.known_role ? hicache_required_fact_errors(fact, route.role) : std::vector<std::string>{};
        if (ordered_fact.causal_tail && (!route.model_fact || route.role != HiCacheFactRole::CacheLifecycleCommit)) continue;
        routed_facts.push_back(RoutedHiCacheFact{
            .fact = std::move(fact),
            .route = route,
            .required_errors = std::move(required_errors),
            .prelude = ordered_fact.prelude,
            .causal_tail = ordered_fact.causal_tail,
        });
    }

    const auto register_control_boundaries = [&](bool prelude) {
        for (const auto & routed : routed_facts) {
            if (routed.prelude != prelude || !routed.route.model_fact || !routed.route.known_role || !routed.required_errors.empty()
                || routed.route.role != HiCacheFactRole::CacheExtendInput)
                continue;
            state.register_prefetch_control_boundary(routed.fact);
        }
    };
    register_control_boundaries(true);
    if (graph.prelude_context_events().empty()) register_control_boundaries(false);

    bool prelude_finalized = graph.prelude_context_events().empty();
    for (const auto & routed : routed_facts) {
        if (!routed.prelude && !prelude_finalized) {
            state.finalize();
            state.begin_formal_window();
            register_control_boundaries(false);
            prelude_finalized = true;
        }
        const auto & fact = routed.fact;
        const auto & route = routed.route;
        if (!route.model_fact) {
            continue;
        }
        if (!route.known_role) {
            (void)core::checked_increment_u64(result.effect_decisions.missing_facts["unknown_state_model_fact"],
                                              "HiCache effect-decision missing-fact count exceeds uint64 range");
            continue;
        }
        if (!routed.required_errors.empty()) {
            std::ranges::for_each(routed.required_errors, [&](const auto & error) {
                (void)core::checked_increment_u64(result.effect_decisions.missing_facts[error],
                                                  "HiCache effect-decision missing-fact count exceeds uint64 range");
            });
            continue;
        }

        state.apply_fact(fact, route.role, !routed.prelude);
    }

    state.finalize();
    auto effect_decisions = state.effect_decision_ledger();
    for (const auto & [reason, count] : result.effect_decisions.missing_facts) {
        auto & merged_count = effect_decisions.missing_facts[reason];
        merged_count = core::checked_add_u64(merged_count, count, "HiCache merged missing-fact count exceeds uint64 range");
    }
    if (!effect_decisions.missing_facts.empty()) effect_decisions.status = "partial";
    result.effect_decisions = std::move(effect_decisions);
    result.phase_work.prefills = state.prefill_work_items();
    result.phase_work.allocator_calls = state.allocator_work_items();
    const auto source_phases = observe_hicache_phases(graph);
    std::map<std::pair<std::string, std::string>, const HiCachePhaseObservation *> source_by_request;
    for (const auto & observation : source_phases.observations) {
        if (observation.request_ids.size() != 1) {
            (void)core::checked_increment_u64(result.phase_work.blockers["source_phase_batch_not_single_request"],
                                              "HiCache phase blocker count exceeds uint64 range");
            continue;
        }
        const auto key = std::make_pair(observation.pid, observation.request_ids.front());
        if (!source_by_request.emplace(key, &observation).second) {
            (void)core::checked_increment_u64(result.phase_work.blockers["duplicate_source_phase_request"],
                                              "HiCache phase blocker count exceeds uint64 range");
        }
    }
    for (auto & prefill : result.phase_work.prefills) {
        const auto found = source_by_request.find({ prefill.pid, prefill.request_id });
        if (found == source_by_request.end()) {
            (void)core::checked_increment_u64(result.phase_work.blockers["source_phase_request_missing"],
                                              "HiCache phase blocker count exceeds uint64 range");
            continue;
        }
        const auto & source = *found->second;
        prefill.logical_input = source.logical_input;
        prefill.source_prefill_token_count = source.prefill_token_count;
        const auto context_tokens = prefill.reusable_prefix_token_count;
        prefill.attention_token_pairs = static_cast<double>(prefill.prefill_token_count)
                                        * (static_cast<double>(context_tokens) + static_cast<double>(prefill.prefill_token_count) / 2.0);
        const auto & phase_cost = config.phase_cost;
        if (phase_cost.enabled) {
            prefill.feature_covered = feature_covered(phase_cost,
                                                      prefill.prefill_token_count,
                                                      context_tokens,
                                                      prefill.attention_token_pairs);
            const auto common_duration = token_curve_duration(
                phase_cost.prefill_common_kernel,
                prefill.prefill_token_count);
            const auto prefix_duration = phase_duration(phase_cost.prefill_prefix_attention,
                                                        prefill.prefill_token_count,
                                                        context_tokens,
                                                        prefill.attention_token_pairs);
            prefill.common_kernel_cost = node_cost(source.prefill_common_kernel_duration_us,
                                                   common_duration,
                                                   source.prefill_common_kernel_node_ids);
            prefill.prefix_attention_cost = node_cost(source.prefill_prefix_attention_duration_us,
                                                      prefix_duration,
                                                      source.prefill_prefix_attention_node_ids);
            auto kernel_nodes = source.prefill_common_kernel_node_ids;
            kernel_nodes.insert(kernel_nodes.end(),
                                source.prefill_prefix_attention_node_ids.begin(),
                                source.prefill_prefix_attention_node_ids.end());
            prefill.kernel_cost = node_cost(source.prefill_kernel_duration_us,
                                            core::checked_add_u64(common_duration,
                                                                  prefix_duration,
                                                                  "HiCache prefill kernel duration exceeds uint64 range"),
                                            kernel_nodes);
            prefill.collective_cost = node_cost(source.prefill_collective_duration_us,
                                                token_curve_duration(phase_cost.prefill_collective,
                                                                     prefill.prefill_token_count),
                                                source.prefill_collective_node_ids);
            prefill.submit_cost = node_cost(source.prefill_submit_cpu_duration_us,
                                            source.prefill_submit_cpu_duration_us,
                                            source.prefill_submit_cpu_node_ids);
        }
        HiCacheDecodeWorkItem decode{
            .logical_input = source.logical_input,
            .pid = source.pid,
            .request_id = prefill.request_id,
            .prompt_token_count = prefill.prompt_token_count,
            .target_page_size = prefill.target_page_size,
            .iteration_count = source.decode_iteration_count,
        };
        if (phase_cost.enabled) {
            decode.feature_covered = prefill.prompt_token_count >= phase_cost.min_decode_context_tokens
                                     && prefill.prompt_token_count <= phase_cost.max_decode_context_tokens;
            decode.source_paged_attention_duration_us = hicache_paged_attention_duration(source.decode_kernel_families);
            if (decode.source_paged_attention_duration_us == 0
                || decode.source_paged_attention_duration_us > source.decode_kernel_duration_us
                || decode.iteration_count == 0) {
                (void)core::checked_increment_u64(result.phase_work.blockers["source_decode_paged_attention_missing"],
                                                  "HiCache phase blocker count exceeds uint64 range");
            }
            else {
                const auto [paged_per_iteration, effective_pages] = decode_paged_attention_duration(
                    phase_cost.decode_paged_attention,
                    decode.prompt_token_count,
                    decode.target_page_size);
                decode.effective_page_count = effective_pages;
                decode.predicted_paged_attention_duration_us = core::checked_multiply_u64(
                    paged_per_iteration,
                    decode.iteration_count,
                    "HiCache Decode paged-attention duration exceeds uint64 range");
                const auto source_non_paged = source.decode_kernel_duration_us - decode.source_paged_attention_duration_us;
                decode.kernel_cost = node_cost(
                    source.decode_kernel_duration_us,
                    core::checked_add_u64(source_non_paged,
                                          decode.predicted_paged_attention_duration_us,
                                          "HiCache Decode kernel duration exceeds uint64 range"),
                    source.decode_kernel_node_ids);
            }
            decode.collective_cost = node_cost(
                source.decode_collective_duration_us,
                core::checked_multiply_u64(phase_duration(phase_cost.decode_collective, 0, decode.prompt_token_count, 0.0),
                                           decode.iteration_count,
                                           "HiCache decode collective duration exceeds uint64 range"),
                source.decode_collective_node_ids);
            decode.submit_cost = node_cost(source.decode_submit_cpu_duration_us,
                                           source.decode_submit_cpu_duration_us,
                                           source.decode_submit_cpu_node_ids);
        }
        result.phase_work.decodes.push_back(std::move(decode));
    }
    if (!source_phases.ready()) {
        (void)core::checked_increment_u64(result.phase_work.blockers["source_phase_observation_not_ready"],
                                          "HiCache phase blocker count exceeds uint64 range");
    }
    result.phase_work.prefill_status = source_phases.ready() && result.phase_work.prefills.size() == source_phases.observations.size() && result.phase_work.blockers.empty()
                                           ? "ready"
                                           : "not_ready";
    result.phase_work.decode_status = result.phase_work.decodes.size() == result.phase_work.prefills.size() && result.phase_work.blockers.empty()
                                          ? "ready"
                                          : "not_ready";
    if (!config.phase_cost.enabled) result.phase_work.cost_status = "disabled";
    else {
        const auto prefill_cost_ready = std::ranges::all_of(result.phase_work.prefills, [](const auto & item) {
            return !item.common_kernel_cost.source_node_ids.empty() && !item.collective_cost.source_node_ids.empty()
                   && !item.submit_cost.source_node_ids.empty();
        });
        const auto decode_cost_ready = std::ranges::all_of(result.phase_work.decodes, [](const auto & item) {
            return !item.kernel_cost.source_node_ids.empty() && !item.collective_cost.source_node_ids.empty()
                   && !item.submit_cost.source_node_ids.empty();
        });
        result.phase_work.cost_status = prefill_cost_ready && decode_cost_ready ? "ready" : "not_ready";
        if (!prefill_cost_ready || !decode_cost_ready)
            (void)core::checked_increment_u64(result.phase_work.blockers["source_phase_cost_nodes_missing"],
                                              "HiCache phase blocker count exceeds uint64 range");
    }
    result.phase_work.status = result.phase_work.prefill_status == "ready" && result.phase_work.decode_status == "ready" ? "ready" : "not_ready";
    result.replay_complete = true;
    return result;
}

} // namespace markov::trace_graph::modules::hicache::model
