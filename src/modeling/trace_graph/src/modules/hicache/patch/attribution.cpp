/**
 * @file
 * @brief Conservative source-effect attribution implementation.
 */
#include "markov/trace_graph/modules/hicache/patch/attribution.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <ranges>
#include <set>
#include <string_view>
#include <unordered_set>

namespace markov::trace_graph::modules::hicache::patch {

namespace attribution_detail {

using model::HiCacheEffectDecision;
using model::HiCacheEffectType;
using model::HiCacheSourceCarrierState;

bool role_matches(std::string_view role, std::initializer_list<std::string_view> expected) {
    return std::ranges::any_of(expected, [&](std::string_view item) { return role == item; });
}

bool same_process(const HiCacheSourceFactNode & fact, const HiCacheSourceFactNode & anchor) { return fact.pid == anchor.pid; }

bool fact_precedes(const HiCacheSourceFactNode & left, const HiCacheSourceFactNode & right) {
    if (left.timestamp_us != right.timestamp_us) return left.timestamp_us < right.timestamp_us;
    if (left.event_index != right.event_index) return left.event_index < right.event_index;
    return left.node_id < right.node_id;
}

const HiCacheSourceFactNode * next_request_opportunity(const HiCacheSourceDagIndex & source, const HiCacheEffectDecision & decision,
                                                       const HiCacheSourceFactNode & anchor) {
    const HiCacheSourceFactNode * next = nullptr;
    if (decision.request_id_provenance.empty()) return next;
    for (size_t node_id : source.nodes_for_request(decision.request_id_provenance)) {
        const auto * candidate = source.fact_node(node_id);
        if (candidate == nullptr || candidate->node_id == anchor.node_id || !same_process(*candidate, anchor) || candidate->fact_role != anchor.fact_role
            || !fact_precedes(anchor, *candidate))
            continue;
        if (next == nullptr || fact_precedes(*candidate, *next)) next = candidate;
    }
    return next;
}

bool role_contract_available(const HiCacheSourceDagIndex & source, std::string_view role) {
    return source.stats().dag_patch_contract_ready || !source.nodes_for_fact_role(role).empty();
}

void sort_unique(std::vector<size_t> & values) {
    std::ranges::sort(values);
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

uint64_t fact_end(const HiCacheSourceFactNode & fact) {
    if (fact.timestamp_us > std::numeric_limits<uint64_t>::max() - fact.duration_us) return std::numeric_limits<uint64_t>::max();
    return fact.timestamp_us + fact.duration_us;
}

bool nested_call(const HiCacheSourceFactNode & child, const HiCacheSourceFactNode & parent) {
    return child.pid == parent.pid && child.tid == parent.tid && child.timestamp_us >= parent.timestamp_us && fact_end(child) <= fact_end(parent);
}

bool contains_object_node(const HiCacheSourceFactNode & fact, uint64_t node_id) {
    return std::ranges::find(fact.operation_node_ids, node_id) != fact.operation_node_ids.end();
}

const HiCacheSourceFactNode * paired_call_end(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & start) {
    const HiCacheSourceFactNode * match = nullptr;
    for (size_t node_id : source.nodes_for_fact_role(start.fact_role)) {
        const auto * candidate = source.fact_node(node_id);
        if (candidate == nullptr || candidate->phase != "end" || candidate->target_id != start.target_id || candidate->pid != start.pid
            || candidate->tid != start.tid || candidate->timestamp_us != start.timestamp_us)
            continue;
        if (match != nullptr) return nullptr;
        match = candidate;
    }
    return match;
}

const HiCacheSourceFactNode * next_process_opportunity(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & anchor) {
    const HiCacheSourceFactNode * next = nullptr;
    for (size_t node_id : source.nodes_for_fact_role(anchor.fact_role)) {
        const auto * candidate = source.fact_node(node_id);
        if (candidate == nullptr || candidate->node_id == anchor.node_id || !same_process(*candidate, anchor) || !fact_precedes(anchor, *candidate))
            continue;
        if (next == nullptr || fact_precedes(*candidate, *next)) next = candidate;
    }
    return next;
}

std::vector<size_t> timing_call_ends_for_object_node(const HiCacheSourceDagIndex & source, std::string_view role, uint64_t object_node_id,
                                                     std::string_view pid, const HiCacheSourceFactNode * lower_bound = nullptr,
                                                     const HiCacheSourceFactNode * upper_bound = nullptr) {
    std::vector<size_t> matches;
    for (size_t node_id : source.nodes_for_fact_role(role)) {
        const auto * start = source.fact_node(node_id);
        if (start == nullptr || start->phase != "start" || start->pid != pid || !contains_object_node(*start, object_node_id)) continue;
        if (lower_bound != nullptr && fact_precedes(*start, *lower_bound)) continue;
        const auto * end = paired_call_end(source, *start);
        if (end == nullptr || end->duration_us == 0) continue;
        if (upper_bound != nullptr && fact_end(*end) > upper_bound->timestamp_us) continue;
        matches.push_back(end->node_id);
    }
    sort_unique(matches);
    return matches;
}

void assign_carrier_nodes(const HiCacheSourceDagIndex & source, std::vector<size_t> carrier_nodes, std::string identity_method, std::string reason,
                          HiCacheSourceAttribution & output) {
    sort_unique(carrier_nodes);
    if (carrier_nodes.empty()) return;
    const std::unordered_set<size_t> carrier_set(carrier_nodes.begin(), carrier_nodes.end());
    output.source_carrier_state = HiCacheSourceCarrierState::Present;
    output.carrier_nodes = std::move(carrier_nodes);
    output.owned_duration_nodes = output.carrier_nodes;
    output.identity_method = std::move(identity_method);
    output.reason = std::move(reason);

    std::optional<size_t> earliest;
    std::optional<size_t> latest;
    for (size_t node_id : output.carrier_nodes) {
        const auto & event = source.graph().event_for_node(node_id);
        if (!earliest || event.ts < source.graph().event_for_node(*earliest).ts) earliest = node_id;
        const auto event_end = fact_end(HiCacheSourceFactNode{ .timestamp_us = event.ts, .duration_us = event.dur });
        const auto latest_end = latest ? fact_end(HiCacheSourceFactNode{
                                       .timestamp_us = source.graph().event_for_node(*latest).ts,
                                       .duration_us = source.graph().event_for_node(*latest).dur })
                                      : 0;
        if (!latest || event_end > latest_end) latest = node_id;
        for (size_t edge_index : source.incoming_edge_ids(node_id)) {
            const auto & edge = source.graph().edge(edge_index);
            if (carrier_set.contains(edge.src)) output.carrier_internal_edges.push_back(edge_index);
            else output.carrier_entry_edges.push_back(edge_index);
        }
        for (size_t edge_index : source.outgoing_edge_ids(node_id)) {
            const auto & edge = source.graph().edge(edge_index);
            if (carrier_set.contains(edge.dst)) output.carrier_internal_edges.push_back(edge_index);
            else output.carrier_exit_edges.push_back(edge_index);
        }
    }
    sort_unique(output.carrier_internal_edges);
    sort_unique(output.carrier_entry_edges);
    sort_unique(output.carrier_exit_edges);
    output.start_anchor = earliest;
    output.completion_anchor = latest;
}

std::optional<std::vector<size_t>> execution_anchors_for_facts(const HiCacheSourceDagIndex & source, const std::vector<size_t> & fact_nodes) {
    std::vector<size_t> anchors;
    anchors.reserve(fact_nodes.size());
    for (size_t fact_node_id : fact_nodes) {
        const auto * fact = source.fact_node(fact_node_id);
        if (fact == nullptr || !fact->execution_anchor_node_id) return std::nullopt;
        const auto anchor = *fact->execution_anchor_node_id;
        if (anchor >= source.graph().node_count() || !source.graph().node(anchor).active) return std::nullopt;
        anchors.push_back(anchor);
    }
    sort_unique(anchors);
    return anchors;
}

void append_candidate(HiCacheSourceAttribution & output, const HiCacheSourceFactNode & fact) {
    if (fact.fact_class == "timing_observation") output.timing_fact_nodes.push_back(fact.node_id);
    else output.control_fact_nodes.push_back(fact.node_id);
}

void collect_request_facts(const HiCacheSourceDagIndex & source, const HiCacheEffectDecision & decision, const HiCacheSourceFactNode & anchor,
                           std::initializer_list<std::string_view> roles, HiCacheSourceAttribution & output) {
    if (decision.request_id_provenance.empty()) return;
    const auto * next_opportunity = next_request_opportunity(source, decision, anchor);
    for (size_t node_id : source.nodes_for_request(decision.request_id_provenance)) {
        const auto * fact = source.fact_node(node_id);
        if (fact == nullptr || !same_process(*fact, anchor) || !role_matches(fact->fact_role, roles)) continue;
        if (fact->fact_class != "source_actual" && fact->fact_class != "timing_observation") continue;
        if (fact_precedes(*fact, anchor)) continue;
        if (next_opportunity != nullptr && !fact_precedes(*fact, *next_opportunity)) continue;
        append_candidate(output, *fact);
    }
}

void expand_operation_chains(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & anchor, HiCacheSourceAttribution & output) {
    std::set<std::string> operation_ids;
    auto collect = [&](const std::vector<size_t> & nodes) {
        for (size_t node_id : nodes) {
            const auto * fact = source.fact_node(node_id);
            if (fact != nullptr && !fact->operation_id.empty()) operation_ids.insert(fact->operation_id);
        }
    };
    collect(output.control_fact_nodes);
    collect(output.timing_fact_nodes);
    for (const auto & operation_id : operation_ids) {
        for (size_t node_id : source.nodes_for_operation(operation_id)) {
            const auto * fact = source.fact_node(node_id);
            if (fact != nullptr && fact->pid == anchor.pid) output.operation_chain_nodes.push_back(node_id);
        }
    }
    sort_unique(output.operation_chain_nodes);
}

bool assign_single_node_carrier(const HiCacheSourceDagIndex & source, size_t fact_node_id, HiCacheSourceAttribution & output) {
    const auto * fact = source.fact_node(fact_node_id);
    if (fact == nullptr || !fact->execution_anchor_node_id) return false;
    const auto node_id = *fact->execution_anchor_node_id;
    if (node_id >= source.graph().node_count() || !source.graph().node(node_id).active) return false;
    output.source_carrier_state = HiCacheSourceCarrierState::Present;
    output.carrier_nodes = { node_id };
    output.owned_duration_nodes = { node_id };
    output.start_anchor = node_id;
    output.completion_anchor = node_id;
    for (size_t edge_index : source.incoming_edge_ids(node_id)) output.carrier_entry_edges.push_back(edge_index);
    for (size_t edge_index : source.outgoing_edge_ids(node_id)) output.carrier_exit_edges.push_back(edge_index);
    output.reason = "unique exact-request timing observation provides a direct source carrier";
    return true;
}

enum class ConsumerAnchorResolution : std::uint8_t { Missing, Ready, Invalid };

bool requires_source_consumer_anchor(HiCacheEffectType effect_type) {
    return effect_type == HiCacheEffectType::Loadback || effect_type == HiCacheEffectType::PrefetchIo
           || effect_type == HiCacheEffectType::PrefetchVisibility
           || effect_type == HiCacheEffectType::CommitCapacityGate;
}

ConsumerAnchorResolution append_target_consumer(const HiCacheSourceDagIndex & source, const HiCacheEffectDecision & decision,
                                                HiCacheSourceAttribution & output) {
    if (!decision.consumer_boundary.source_node_id) return ConsumerAnchorResolution::Missing;
    const auto * fact = source.fact_node(*decision.consumer_boundary.source_node_id);
    if (fact == nullptr || (!decision.consumer_boundary.source_fact_role.empty() && fact->fact_role != decision.consumer_boundary.source_fact_role))
        return ConsumerAnchorResolution::Invalid;
    const auto anchor = decision.consumer_boundary.execution_anchor_node_id ? decision.consumer_boundary.execution_anchor_node_id
                                                                             : fact->execution_anchor_node_id;
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

void classify_prefetch_io(const HiCacheSourceDagIndex & source, HiCacheSourceAttribution & output) {
    if (output.timing_fact_nodes.size() == 1) {
        const auto fact_node_id = output.timing_fact_nodes.front();
        const auto * fact = source.fact_node(fact_node_id);
        if (fact != nullptr && fact->execution_anchor_node_id && source.graph().node(*fact->execution_anchor_node_id).duration > 0) {
            if (!assign_single_node_carrier(source, fact_node_id, output)) {
                output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
                output.reason = "prefetch timing observation has no active executable anchor";
                return;
            }
            output.identity_method = "request_id+pid";
            output.evidence.push_back("prefetch_io_observed");
            if (!output.operation_chain_nodes.empty()) output.evidence.push_back("operation_id+pid");
            return;
        }
        output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
        output.reason = "prefetch timing observation has no executable source carrier";
        return;
    }
    if (!output.control_fact_nodes.empty()) {
        const bool reached_progress_boundary = std::ranges::any_of(output.control_fact_nodes, [&](size_t node_id) {
            const auto * fact = source.fact_node(node_id);
            return fact != nullptr && fact->fact_role == "prefetch_progress_observed";
        });
        if (reached_progress_boundary) {
            output.source_carrier_state = HiCacheSourceCarrierState::Absent;
            output.identity_method = "request_id+pid+terminal_progress";
            output.reason = "source prefetch reached its progress boundary without a storage I/O timing carrier";
            return;
        }
    }
    if (role_contract_available(source, "prefetch_io_observed") && !output.consumer_anchors.empty()) {
        output.source_carrier_state = HiCacheSourceCarrierState::Absent;
        output.identity_method = "request_id+pid+consumer";
        output.reason = "source request reached its consumer without a prefetch storage I/O operation";
        return;
    }
    if (output.timing_fact_nodes.size() > 1) {
        output.source_carrier_state = HiCacheSourceCarrierState::Ambiguous;
        output.identity_method = "request_id+pid";
        output.reason = "multiple prefetch timing observations match one source opportunity";
        return;
    }
    output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
    output.identity_method = output.control_fact_nodes.empty() ? "none" : "request_id+pid";
    output.reason =
        output.control_fact_nodes.empty() ? "source prefetch operation evidence is missing" : "prefetch control evidence exists without a timing carrier";
}

std::vector<const HiCacheSourceFactNode *> nested_role_facts(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & anchor,
                                                             std::string_view role) {
    std::vector<const HiCacheSourceFactNode *> matches;
    for (size_t node_id : source.nodes_for_fact_role(role)) {
        const auto * fact = source.fact_node(node_id);
        if (fact != nullptr && nested_call(*fact, anchor)) matches.push_back(fact);
    }
    std::ranges::sort(matches, [](const auto * left, const auto * right) {
        if (left->timestamp_us != right->timestamp_us) return left->timestamp_us < right->timestamp_us;
        return left->node_id < right->node_id;
    });
    return matches;
}

std::vector<const HiCacheSourceFactNode *> commit_d2h_enqueues(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & anchor) {
    auto matches = nested_role_facts(source, anchor, "commit_device_to_host_enqueue_observed");
    matches.erase(std::remove_if(matches.begin(), matches.end(), [](const auto * fact) { return fact->phase != "end"; }), matches.end());
    return matches;
}

void classify_loadback(const HiCacheSourceDagIndex & source, const HiCacheEffectDecision & decision, const HiCacheSourceFactNode & anchor,
                       HiCacheSourceAttribution & output) {
    collect_request_facts(source,
                          decision,
                          anchor,
                          { "loadback_decision_observed", "request_admission_observed", "capacity_result_observed", "insert_result_observed" },
                          output);
    std::vector<const HiCacheSourceFactNode *> decisions;
    for (size_t node_id : output.control_fact_nodes) {
        const auto * fact = source.fact_node(node_id);
        if (fact != nullptr && fact->fact_role == "loadback_decision_observed") decisions.push_back(fact);
    }
    if (decisions.empty()) {
        const bool complete_contract = role_contract_available(source, "loadback_decision_observed");
        output.source_carrier_state =
            complete_contract && !output.consumer_anchors.empty() ? HiCacheSourceCarrierState::Absent : HiCacheSourceCarrierState::Unobservable;
        output.identity_method = output.control_fact_nodes.empty() ? "none" : "request_id+pid+opportunity_window";
        output.reason = !complete_contract                 ? "profiling contract does not expose loadback decisions"
                        : !output.consumer_anchors.empty() ? "source request reached its consumer without a loadback operation"
                                                           : "source request has no explicit loadback decision or consumer";
        return;
    }
    if (decisions.size() != 1) {
        output.source_carrier_state = HiCacheSourceCarrierState::Ambiguous;
        output.identity_method = "request_id+pid+opportunity_window";
        output.reason = "multiple loadback decisions match one source opportunity";
        return;
    }
    const auto & source_decision = *decisions.front();
    output.evidence.push_back("loadback_decision_observed");
    if (source_decision.effective_token_count == 0) {
        output.source_carrier_state = HiCacheSourceCarrierState::Absent;
        output.identity_method = "request_id+pid+opportunity_window";
        output.reason = "source loadback decision explicitly returned no effective tokens";
        return;
    }
    if (!source_decision.object_node_id) {
        output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
        output.identity_method = "request_id+pid+opportunity_window";
        output.reason = "source loadback decision lacks a tree-node identity";
        return;
    }
    const HiCacheSourceFactNode * upper_bound = nullptr;
    if (decision.consumer_boundary.source_node_id) upper_bound = source.fact_node(*decision.consumer_boundary.source_node_id);
    if (upper_bound == nullptr) upper_bound = next_process_opportunity(source, anchor);
    auto timing_nodes =
        timing_call_ends_for_object_node(source, "loadback_io_observed", *source_decision.object_node_id, anchor.pid, &source_decision, upper_bound);
    if (timing_nodes.size() != 1) {
        output.source_carrier_state = timing_nodes.empty() ? HiCacheSourceCarrierState::Unobservable : HiCacheSourceCarrierState::Ambiguous;
        output.identity_method = "request_id+pid+opportunity_window+tree_node_id";
        output.reason = timing_nodes.empty() ? "loadback decision has no matching H2D timing call" : "loadback decision matches multiple H2D timing calls";
        return;
    }
    output.evidence.push_back("loadback_io_observed");
    output.timing_fact_nodes = timing_nodes;
    const auto timing_anchors = execution_anchors_for_facts(source, timing_nodes);
    if (!timing_anchors) {
        output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
        output.identity_method = "request_id+pid+opportunity_window+tree_node_id";
        output.reason = "loadback timing observation has no proven executable source anchor";
        return;
    }
    assign_carrier_nodes(source,
                         *timing_anchors,
                         "request_id+pid+opportunity_window+tree_node_id+call_identity",
                         "loadback decision and H2D timing call share one tree-node identity",
                         output);
}

void classify_prefetch_visibility(const HiCacheSourceDagIndex & source, const HiCacheEffectDecision & decision, const HiCacheSourceFactNode & anchor,
                                  HiCacheSourceAttribution & output) {
    collect_request_facts(
        source,
        decision,
        anchor,
        { "prefetch_intent_observed", "prefetch_decision_observed", "prefetch_progress_observed", "prefetch_io_observed", "request_admission_observed" },
        output);
    expand_operation_chains(source, anchor, output);
    const bool consumer_ready = !output.consumer_anchors.empty();
    std::vector<size_t> progress_nodes;
    bool source_prefetch_observed = false;
    bool progress_result_missing = false;
    bool blocking_progress_observed = false;
    for (size_t node_id : output.control_fact_nodes) {
        const auto * fact = source.fact_node(node_id);
        if (fact == nullptr) continue;
        if (fact->fact_role == "prefetch_progress_observed") {
            progress_nodes.push_back(node_id);
            if (!fact->progress_ready) progress_result_missing = true;
            else if (!*fact->progress_ready) blocking_progress_observed = true;
        }
        if (fact->fact_role == "prefetch_intent_observed" || fact->fact_role == "prefetch_decision_observed") source_prefetch_observed = true;
    }
    sort_unique(progress_nodes);
    if (progress_nodes.empty() && !source_prefetch_observed) {
        output.source_carrier_state = HiCacheSourceCarrierState::Absent;
        output.identity_method = consumer_ready ? "request_id+pid+consumer" : "request_id+pid";
        output.reason = "source request has no prefetch visibility control operation";
        return;
    }
    if (progress_nodes.empty()) {
        output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
        output.identity_method = "request_id+pid";
        output.reason = "source prefetch has no exact progress observation";
        return;
    }
    if (progress_result_missing) {
        output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
        output.identity_method = "request_id+pid+progress_result";
        output.reason = "source prefetch progress observations do not expose their blocking result";
        return;
    }
    if (!blocking_progress_observed) {
        output.source_carrier_state = HiCacheSourceCarrierState::Absent;
        output.identity_method = "request_id+pid+progress_result";
        output.evidence.push_back("prefetch_progress_observed");
        output.reason = "all source prefetch progress checks completed without blocking the consumer";
        return;
    }
    if (!consumer_ready) {
        output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
        output.identity_method = "target_consumer_boundary";
        output.reason = "blocking source prefetch progress has no canonical consumer in the source DAG";
        return;
    }
    const auto progress_anchors = execution_anchors_for_facts(source, progress_nodes);
    if (!progress_anchors) {
        output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
        output.identity_method = "request_id+pid+blocking_progress+consumer";
        output.reason = "prefetch progress observation has no proven executable source anchor";
        return;
    }
    output.source_carrier_state = HiCacheSourceCarrierState::Present;
    output.identity_method = "request_id+pid+blocking_progress+consumer";
    output.evidence.push_back("prefetch_progress_observed");
    output.carrier_nodes = *progress_anchors;
    output.start_anchor = output.carrier_nodes.front();
    output.completion_anchor = output.carrier_nodes.back();
    output.reason = "exact request progress checks guard the canonical cache-extend consumer";
}

void classify_commit_d2h(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & anchor, HiCacheSourceAttribution & output) {
    if (!role_contract_available(source, "commit_device_to_host_enqueue_observed")) {
        output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
        output.reason = "profiling contract does not expose D2H commit enqueue calls";
        return;
    }
    const auto enqueues = commit_d2h_enqueues(source, anchor);
    for (const auto * enqueue : enqueues) output.control_fact_nodes.push_back(enqueue->node_id);
    if (enqueues.empty()) {
        output.source_carrier_state = HiCacheSourceCarrierState::Absent;
        output.identity_method = "lifecycle_call_containment";
        output.reason = "source lifecycle completed without a nested D2H commit enqueue";
        return;
    }

    std::vector<size_t> timing_nodes;
    bool missing_identity = false;
    bool missing_timing = false;
    for (const auto * enqueue : enqueues) {
        if (enqueue->effective_token_count == 0) continue;
        if (!enqueue->object_node_id) {
            missing_identity = true;
            continue;
        }
        auto matches = timing_call_ends_for_object_node(source, "commit_device_to_host_io_observed", *enqueue->object_node_id, anchor.pid);
        if (matches.empty()) missing_timing = true;
        timing_nodes.insert(timing_nodes.end(), matches.begin(), matches.end());
    }
    sort_unique(timing_nodes);
    if (missing_identity || missing_timing || timing_nodes.empty()) {
        output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
        output.identity_method = "lifecycle_call_containment+tree_node_id";
        output.reason = missing_identity ? "nested D2H enqueue lacks a tree-node identity" : "nested D2H enqueue has no matching timing call";
        return;
    }
    output.timing_fact_nodes = timing_nodes;
    output.evidence.push_back("commit_device_to_host_enqueue_observed");
    output.evidence.push_back("commit_device_to_host_io_observed");
    const auto timing_anchors = execution_anchors_for_facts(source, timing_nodes);
    if (!timing_anchors) {
        output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
        output.identity_method = "lifecycle_call_containment+tree_node_id";
        output.reason = "D2H timing observation has no proven executable source anchor";
        return;
    }
    assign_carrier_nodes(source,
                         *timing_anchors,
                         "lifecycle_call_containment+tree_node_id+call_identity",
                         "nested commit enqueue and D2H timing calls share tree-node identities",
                         output);
}

std::vector<const HiCacheSourceFactNode *> capacity_release_calls(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & anchor) {
    std::set<uint64_t> object_node_ids;
    for (const auto * enqueue : commit_d2h_enqueues(source, anchor)) {
        if (enqueue->effective_token_count > 0 && enqueue->object_node_id) object_node_ids.insert(*enqueue->object_node_id);
    }
    std::vector<const HiCacheSourceFactNode *> releases;
    for (size_t node_id : source.nodes_for_fact_role("commit_capacity_release_observed")) {
        const auto * start = source.fact_node(node_id);
        if (start == nullptr || start->phase != "start" || start->pid != anchor.pid) continue;
        const bool owns_node = std::ranges::any_of(object_node_ids, [&](uint64_t object_node_id) { return contains_object_node(*start, object_node_id); });
        if (!owns_node) continue;
        if (const auto * end = paired_call_end(source, *start)) releases.push_back(end);
    }
    std::ranges::sort(releases, [](const auto * left, const auto * right) {
        if (left->timestamp_us != right->timestamp_us) return left->timestamp_us < right->timestamp_us;
        return left->node_id < right->node_id;
    });
    return releases;
}

const HiCacheSourceFactNode * paired_tail_call_end(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & start) {
    const HiCacheSourceFactNode * match = nullptr;
    for (const auto & candidate : source.tail_context_facts()) {
        if (candidate.fact_role != start.fact_role || candidate.phase != "end" || candidate.target_id != start.target_id || candidate.pid != start.pid
            || candidate.tid != start.tid || candidate.timestamp_us != start.timestamp_us)
            continue;
        if (match != nullptr) return nullptr;
        match = &candidate;
    }
    return match;
}

bool all_capacity_releases_in_tail(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & anchor) {
    std::set<uint64_t> expected;
    for (const auto * enqueue : commit_d2h_enqueues(source, anchor)) {
        if (enqueue->effective_token_count > 0 && enqueue->object_node_id) expected.insert(*enqueue->object_node_id);
    }
    if (expected.empty()) return false;

    std::set<uint64_t> closed;
    for (const auto & start : source.tail_context_facts()) {
        if (start.fact_role != "commit_capacity_release_observed" || start.phase != "start" || start.pid != anchor.pid) continue;
        const auto * end = paired_tail_call_end(source, start);
        if (end == nullptr || end->duration_us == 0) continue;
        for (uint64_t object_node_id : start.operation_node_ids) {
            if (expected.contains(object_node_id)) closed.insert(object_node_id);
        }
    }
    return closed == expected;
}

void classify_commit_h2s(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & anchor, HiCacheSourceAttribution & output) {
    if (!role_contract_available(source, "commit_capacity_release_observed")) {
        output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
        output.reason = "profiling contract does not expose commit ACK-to-storage boundaries";
        return;
    }
    if (commit_d2h_enqueues(source, anchor).empty()) {
        output.source_carrier_state = HiCacheSourceCarrierState::Absent;
        output.identity_method = "lifecycle_call_containment";
        output.reason = "source lifecycle completed without a D2H commit that could enqueue storage I/O";
        return;
    }
    const auto releases = capacity_release_calls(source, anchor);
    if (releases.empty()) {
        if (all_capacity_releases_in_tail(source, anchor)) {
            output.source_carrier_state = HiCacheSourceCarrierState::Absent;
            output.identity_method = "lifecycle_call_containment+tree_node_id+post_window_call_identity";
            output.reason = "source commit ACK and storage carrier complete after the measured window";
            output.evidence.push_back("post_window_commit_capacity_release_observed");
            return;
        }
        output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
        output.identity_method = "lifecycle_call_containment+tree_node_id";
        output.reason = "source lifecycle has no matching commit ACK boundary";
        return;
    }

    std::vector<const HiCacheSourceFactNode *> enqueues;
    for (const auto * release : releases) {
        for (const auto * enqueue : nested_role_facts(source, *release, "writeback_enqueue_observed")) {
            if (enqueue->phase == "end") enqueues.push_back(enqueue);
        }
    }
    if (enqueues.empty()) {
        output.source_carrier_state = HiCacheSourceCarrierState::Absent;
        output.identity_method = "tree_node_id+ack_call_containment";
        output.reason = "source commit ACK completed without a nested storage enqueue";
        return;
    }

    std::vector<size_t> timing_nodes;
    bool missing_operation_id = false;
    bool ambiguous_operation = false;
    for (const auto * enqueue : enqueues) {
        output.control_fact_nodes.push_back(enqueue->node_id);
        if (enqueue->operation_id.empty()) {
            missing_operation_id = true;
            continue;
        }
        std::vector<size_t> matches;
        for (size_t node_id : source.nodes_for_operation(enqueue->operation_id)) {
            const auto * fact = source.fact_node(node_id);
            if (fact != nullptr && fact->pid == anchor.pid && fact->fact_role == "writeback_io_observed" && fact->phase == "end" && fact->duration_us > 0)
                matches.push_back(node_id);
        }
        sort_unique(matches);
        if (matches.size() > 1) ambiguous_operation = true;
        timing_nodes.insert(timing_nodes.end(), matches.begin(), matches.end());
    }
    sort_unique(timing_nodes);
    if (missing_operation_id || timing_nodes.empty() || ambiguous_operation) {
        output.source_carrier_state = ambiguous_operation ? HiCacheSourceCarrierState::Ambiguous : HiCacheSourceCarrierState::Unobservable;
        output.identity_method = "tree_node_id+ack_call_containment+operation_id";
        if (missing_operation_id) output.reason = "storage enqueue lacks an operation identity";
        else if (ambiguous_operation) output.reason = "storage operation identity matches multiple timing calls";
        else output.reason = "storage enqueue has no matching H2S timing call";
        return;
    }
    output.timing_fact_nodes = timing_nodes;
    output.evidence.push_back("commit_capacity_release_observed");
    output.evidence.push_back("writeback_enqueue_observed");
    output.evidence.push_back("writeback_io_observed");
    const auto timing_anchors = execution_anchors_for_facts(source, timing_nodes);
    if (!timing_anchors) {
        output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
        output.identity_method = "tree_node_id+ack_call_containment+operation_id";
        output.reason = "H2S timing observation has no proven executable source anchor";
        return;
    }
    assign_carrier_nodes(source,
                         *timing_anchors,
                         "tree_node_id+ack_call_containment+operation_id+pid",
                         "commit ACK contains a storage enqueue joined to one H2S timing operation",
                         output);
}

HiCacheSourceAttribution attribute_one(const HiCacheSourceDagIndex & source, const HiCacheEffectDecision & decision) {
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

    switch (decision.effect_type) {
    case HiCacheEffectType::PrefetchIo:
        collect_request_facts(source,
                              decision,
                              *anchor,
                              { "prefetch_intent_observed", "prefetch_decision_observed", "prefetch_progress_observed", "prefetch_io_observed" },
                              output);
        expand_operation_chains(source, *anchor, output);
        classify_prefetch_io(source, output);
        break;
    case HiCacheEffectType::PrefetchVisibility:
        classify_prefetch_visibility(source, decision, *anchor, output);
        break;
    case HiCacheEffectType::CommitDeviceToHost:
        classify_commit_d2h(source, *anchor, output);
        break;
    case HiCacheEffectType::CommitHostToStorage:
        classify_commit_h2s(source, *anchor, output);
        break;
    case HiCacheEffectType::CommitCapacityGate: {
        if (!role_contract_available(source, "commit_capacity_release_observed")) {
            output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
            output.reason = "profiling contract does not expose commit capacity-release calls";
            break;
        }
        if (commit_d2h_enqueues(source, *anchor).empty()) {
            output.source_carrier_state = HiCacheSourceCarrierState::Absent;
            output.identity_method = "lifecycle_call_containment";
            output.reason = "source lifecycle completed without a D2H commit requiring a capacity gate";
            break;
        }
        for (const auto * release : capacity_release_calls(source, *anchor)) output.control_fact_nodes.push_back(release->node_id);
        if (output.control_fact_nodes.empty()) {
            if (all_capacity_releases_in_tail(source, *anchor)) {
                output.source_carrier_state = HiCacheSourceCarrierState::Absent;
                output.identity_method = "lifecycle_call_containment+tree_node_id+post_window_call_identity";
                output.reason = "source capacity release completes after the measured window";
                output.evidence.push_back("post_window_commit_capacity_release_observed");
                break;
            }
            output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
            output.identity_method = "lifecycle_call_containment+tree_node_id";
            output.reason = "D2H commit has no matching capacity-release call";
            break;
        }
        if (output.consumer_anchors.empty()) {
            output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
            output.identity_method = "lifecycle_call_containment+tree_node_id+call_identity";
            output.reason = "capacity release is observed but the target consumer boundary is unavailable";
            break;
        }
        const auto release_anchors = execution_anchors_for_facts(source, output.control_fact_nodes);
        if (!release_anchors) {
            output.source_carrier_state = HiCacheSourceCarrierState::Unobservable;
            output.identity_method = "lifecycle_call_containment+tree_node_id+call_identity";
            output.reason = "capacity-release observation has no proven executable source anchor";
            break;
        }
        output.source_carrier_state = HiCacheSourceCarrierState::Present;
        output.carrier_nodes = *release_anchors;
        output.identity_method = "lifecycle_call_containment+tree_node_id+call_identity+target_consumer";
        output.reason = "capacity release call guards the next target-derived canonical consumer";
        break;
    }
    case HiCacheEffectType::Loadback:
        classify_loadback(source, decision, *anchor, output);
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

HiCacheSourceAttributionCatalog build_hicache_source_attribution(const HiCacheSourceDagIndex & source, const model::HiCacheEffectDecisionLedger & decisions) {
    HiCacheSourceAttributionCatalog catalog;
    catalog.records.reserve(decisions.decisions.size());
    for (const auto & decision : decisions.decisions) {
        auto record = attribution_detail::attribute_one(source, decision);
        const auto state = model::hicache_source_carrier_state_name(record.source_carrier_state);
        (void)core::checked_increment_u64(catalog.counts_by_source_carrier_state[state], "HiCache source carrier-state count exceeds uint64 range");
        (void)core::checked_increment_u64(catalog.counts_by_effect_type[model::hicache_effect_type_name(record.effect_type)],
                                          "HiCache source attribution effect-type count exceeds uint64 range");
        if (record.source_carrier_state == model::HiCacheSourceCarrierState::Unobservable
            || record.source_carrier_state == model::HiCacheSourceCarrierState::Ambiguous)
            (void)core::checked_increment_u64(catalog.blocker_counts[record.reason], "HiCache source attribution blocker count exceeds uint64 range");
        catalog.records.push_back(std::move(record));
    }
    if (catalog.records.empty()) catalog.status = "no_effect_decisions";
    else if (catalog.unresolved_count() == 0) catalog.status = "ready";
    else if (catalog.attributed_count() > 0) catalog.status = "partial";
    else catalog.status = "blocked";
    return catalog;
}

} // namespace markov::trace_graph::modules::hicache::patch
