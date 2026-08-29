/**
 * @file
 * @brief HiCache fact scanning, target-state replay, and Debug summary convergence.
 */
#include "markov/trace_graph/modules/hicache/model/state.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::model {

using core::DagGraph;
using frontend::HiCacheConfig;

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
    HiCacheState state(config);

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
    result.replay_complete = true;
    return result;
}

} // namespace markov::trace_graph::modules::hicache::model
