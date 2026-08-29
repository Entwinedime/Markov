/**
 * @file
 * @brief Conservative source-effect attribution implementation.
 */
#include "markov/trace_graph/modules/hicache/patch/attribution.hpp"

#include "attribution_common.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <map>

namespace markov::trace_graph::modules::hicache::patch {

namespace attribution_detail {

using model::HiCacheEffectDecision;
using model::HiCacheEffectType;
using model::HiCacheSourceCarrierState;

enum class ConsumerAnchorResolution : std::uint8_t { Missing, Ready, Invalid };

bool requires_source_consumer_anchor(HiCacheEffectType effect_type) {
    return effect_type == HiCacheEffectType::Loadback || effect_type == HiCacheEffectType::PrefetchIo || effect_type == HiCacheEffectType::PrefetchVisibility
           || effect_type == HiCacheEffectType::CommitCapacityGate;
}

ConsumerAnchorResolution append_target_consumer(const HiCacheSourceDagIndex & source, const HiCacheEffectDecision & decision,
                                                HiCacheSourceAttribution & output) {
    if (!decision.consumer_boundary.source_node_id) return ConsumerAnchorResolution::Missing;
    const auto * fact = source.fact_node(*decision.consumer_boundary.source_node_id);
    if (fact == nullptr || (!decision.consumer_boundary.source_fact_role.empty() && fact->fact_role != decision.consumer_boundary.source_fact_role))
        return ConsumerAnchorResolution::Invalid;
    auto anchor = decision.consumer_boundary.execution_anchor_node_id ? decision.consumer_boundary.execution_anchor_node_id : fact->execution_anchor_node_id;
    if (!anchor) anchor = source.cpu_boundary_at_or_after(fact->pid, fact->tid, fact_boundary(*fact));
    if (!anchor) return ConsumerAnchorResolution::Missing;
    const auto node_id = *anchor;
    if (node_id >= source.graph().node_count() || !source.graph().node(node_id).active) return ConsumerAnchorResolution::Invalid;
    output.consumer_anchors.push_back(node_id);
    sort_unique(output.consumer_anchors);
    output.consumer_anchor_method = "target_canonical_consumer";
    return ConsumerAnchorResolution::Ready;
}

bool append_unique_sequential_successor(const HiCacheSourceDagIndex & source, size_t source_node_id, HiCacheSourceAttribution & output) {
    const auto * source_fact = source.fact_node(source_node_id);
    if (source_fact == nullptr || !source_fact->execution_anchor_node_id) {
        output.consumer_anchor_method = "source_fact_has_no_executable_anchor";
        return false;
    }
    const auto anchor_node_id = *source_fact->execution_anchor_node_id;
    std::vector<size_t> successors;
    for (size_t edge_index : source.outgoing_edge_ids(anchor_node_id)) {
        const auto & edge = source.graph().edge(edge_index);
        if (edge.kind == core::DagEdgeKind::Sequential) successors.push_back(edge.dst);
    }
    sort_unique(successors);
    if (successors.size() != 1) {
        output.consumer_anchor_method = successors.empty() ? "missing" : "ambiguous_sequential_successor";
        return false;
    }
    output.consumer_anchors.push_back(successors.front());
    sort_unique(output.consumer_anchors);
    output.consumer_anchor_method = "unique_original_sequential_successor";
    return true;
}

HiCacheSourceAttribution attribute_one(const HiCacheSourceDagIndex & source, const HiCacheEffectDecision & decision,
                                       const HiCacheIoOperationLedger & operations) {
    HiCacheSourceAttribution output{
        .effect_id = decision.effect_key,
        .effect_type = decision.effect_type,
        .target_effect_state = decision.target_effect_state,
        .source_fact_node_id = decision.source_node_id,
    };
    const auto * anchor = source.fact_node(decision.source_node_id);
    if (anchor == nullptr || anchor->fact_role != decision.source_fact_role) {
        output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
        output.reason = "source opportunity anchor is missing from the semantic DAG index";
        return output;
    }
    output.source_execution_anchor_node_id = anchor->execution_anchor_node_id;
    if (!output.source_execution_anchor_node_id)
        output.source_execution_anchor_node_id = source.cpu_boundary_at_or_before(anchor->pid, anchor->tid, fact_boundary(*anchor));
    if (!output.source_execution_anchor_node_id)
        output.source_execution_anchor_node_id = source.cpu_boundary_at_or_after(anchor->pid, anchor->tid, fact_boundary(*anchor));
    output.evidence.push_back("source_opportunity_anchor");
    const auto consumer_resolution = append_target_consumer(source, decision, output);
    if (consumer_resolution == ConsumerAnchorResolution::Invalid) {
        output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
        output.consumer_anchor_method = "invalid_target_canonical_consumer";
        output.reason = "target consumer boundary does not resolve to its declared active source fact";
        return output;
    }
    if (consumer_resolution == ConsumerAnchorResolution::Missing) {
        if (requires_source_consumer_anchor(decision.effect_type)) (void)append_unique_sequential_successor(source, decision.source_node_id, output);
        else output.consumer_anchor_method = "asynchronous_effect_no_source_consumer";
    }
    if (decision.effect_type == HiCacheEffectType::CommitHostToStorage) {
        output.consumer_anchors.clear();
        output.consumer_anchor_method = "target_h2s_background_without_foreground_consumer";
        output.evidence.push_back("host_storage_write_remains_background");
    }

    switch (decision.effect_type) {
    case HiCacheEffectType::PrefetchIo:
        classify_io_from_ledger(source, operations, decision, *anchor, output);
        break;
    case HiCacheEffectType::PrefetchVisibility:
        output.source_carrier_state = HiCacheSourceCarrierState::Absent;
        output.reason = "prefetch visibility is represented by the prefetch-to-load-to-consumer I/O chain; polling CPU cost remains residual";
        break;
    case HiCacheEffectType::CommitDeviceToHost:
        classify_commit_d2h_from_ledger(source, operations, decision, *anchor, output);
        break;
    case HiCacheEffectType::CommitHostToStorage:
        classify_commit_h2s_from_ledger(source, operations, *anchor, output);
        break;
    case HiCacheEffectType::CommitCapacityGate: {
        if (!role_contract_available(source, "commit_capacity_release_observed")) {
            output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
            output.reason = "profiling contract does not expose commit capacity-release calls";
            break;
        }
        if (commit_d2h_enqueues(source, *anchor).empty()) {
            output.source_carrier_state = HiCacheSourceCarrierState::Absent;
            output.reason = "source lifecycle completed without a D2H commit requiring a capacity gate";
            break;
        }
        for (const auto * release : capacity_release_calls(source, *anchor)) output.control_fact_nodes.push_back(release->node_id);
        if (output.control_fact_nodes.empty()) {
            if (all_capacity_releases_in_tail(source, *anchor)) {
                output.source_carrier_state = HiCacheSourceCarrierState::Absent;
                output.reason = "source capacity release completes after the measured window";
                output.evidence.push_back("post_window_commit_capacity_release_observed");
                break;
            }
            output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
            output.reason = "D2H commit has no matching capacity-release call";
            break;
        }
        if (output.consumer_anchors.empty() && decision.target_effect_state != model::HiCacheTargetEffectState::NotRequired) {
            output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
            output.reason = "capacity release is observed but the target consumer boundary is unavailable";
            break;
        }
        if (!output.source_execution_anchor_node_id) {
            output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
            output.reason = "capacity-release observation has no executable lifecycle boundary";
            break;
        }
        output.source_carrier_state = HiCacheSourceCarrierState::Present;
        if (output.consumer_anchors.empty()) {
            output.reason = "exact source capacity-release control fact is present, while the target explicitly omits the capacity dependency";
        }
        else {
            output.reason = "exact capacity-release control fact guards the next target-derived canonical consumer without claiming CPU duration ownership";
        }
        break;
    }
    case HiCacheEffectType::Loadback:
        classify_io_from_ledger(source, operations, decision, *anchor, output);
        break;
    }

    sort_unique(output.control_fact_nodes);
    sort_unique(output.timing_fact_nodes);
    return output;
}

} // namespace attribution_detail

uint64_t HiCacheSourceAttributionCatalog::attributed_count() const {
    uint64_t count = 0;
    for (const auto & record : records) {
        if (record.source_carrier_state == model::HiCacheSourceCarrierState::Present || record.source_carrier_state == model::HiCacheSourceCarrierState::Absent)
            (void)core::checked_increment_u64(count, "HiCache attributed effect count exceeds uint64 range");
    }
    return count;
}

uint64_t HiCacheSourceAttributionCatalog::unresolved_count() const { return static_cast<uint64_t>(records.size()) - attributed_count(); }

HiCacheSourceAttributionCatalog build_hicache_source_attribution(const HiCacheSourceDagIndex & source, const model::HiCacheEffectDecisionLedger & decisions,
                                                                 const HiCacheIoOperationLedger & operations) {
    HiCacheSourceAttributionCatalog catalog;
    catalog.records.reserve(decisions.decisions.size());
    for (const auto & decision : decisions.decisions) {
        auto record = attribution_detail::attribute_one(source, decision, operations);
        const auto state = model::hicache_source_carrier_state_name(record.source_carrier_state);
        (void)core::checked_increment_u64(catalog.counts_by_source_carrier_state[state], "HiCache source carrier-state count exceeds uint64 range");
        (void)core::checked_increment_u64(catalog.counts_by_effect_type[model::hicache_effect_type_name(record.effect_type)],
                                          "HiCache source attribution effect-type count exceeds uint64 range");
        if (record.source_carrier_state == model::HiCacheSourceCarrierState::Unobservable
            || record.source_carrier_state == model::HiCacheSourceCarrierState::Ambiguous)
            (void)core::checked_increment_u64(catalog.blocker_counts[record.reason], "HiCache source attribution blocker count exceeds uint64 range");
        catalog.records.push_back(std::move(record));
    }
    std::map<std::string, uint64_t> d2h_claim_counts;
    for (const auto & record : catalog.records) {
        if (record.effect_type != model::HiCacheEffectType::CommitDeviceToHost) continue;
        for (const auto & record_id : record.io_operation_record_ids)
            (void)core::checked_increment_u64(d2h_claim_counts[record_id], "HiCache D2H ledger claim count exceeds uint64 range");
    }
    for (const auto & operation : operations.records) {
        if (operation.kind != HiCacheIoOperationKind::WriteDeviceToHost || !attribution_detail::ledger_record_ready(operation)) continue;
        (void)core::checked_increment_u64(catalog.d2h_ready_record_count, "HiCache ready D2H ledger count exceeds uint64 range");
        const auto count = d2h_claim_counts[operation.record_id];
        if (count == 1) (void)core::checked_increment_u64(catalog.d2h_claimed_record_count, "HiCache claimed D2H ledger count exceeds uint64 range");
        else if (count == 0) (void)core::checked_increment_u64(catalog.d2h_unclaimed_record_count, "HiCache unclaimed D2H ledger count exceeds uint64 range");
        else (void)core::checked_increment_u64(catalog.d2h_multiply_claimed_record_count, "HiCache multiply claimed D2H ledger count exceeds uint64 range");
    }
    if (catalog.d2h_unclaimed_record_count > 0)
        catalog.blocker_counts["ready D2H ledger record has no lifecycle attribution"] = catalog.d2h_unclaimed_record_count;
    if (catalog.d2h_multiply_claimed_record_count > 0)
        catalog.blocker_counts["ready D2H ledger record is attributed to multiple lifecycles"] = catalog.d2h_multiply_claimed_record_count;
    if (catalog.records.empty()) catalog.status = "no_effect_decisions";
    else if (catalog.unresolved_count() == 0 && catalog.d2h_unclaimed_record_count == 0 && catalog.d2h_multiply_claimed_record_count == 0)
        catalog.status = "ready";
    else if (catalog.attributed_count() > 0) catalog.status = "partial";
    else catalog.status = "blocked";
    return catalog;
}

} // namespace markov::trace_graph::modules::hicache::patch
