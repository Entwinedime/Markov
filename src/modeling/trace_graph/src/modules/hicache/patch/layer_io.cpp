#include "markov/trace_graph/modules/hicache/patch/layer_io.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>
#include <tuple>

namespace markov::trace_graph::modules::hicache::patch {

std::vector<HiCacheLayerTransferTemplate> observe_hicache_layer_transfers(const HiCacheSourceDagIndex & source, const HiCacheLayerWaitObservation & waits,
                                                                          const HiCacheRewriteDecision & donor) {
    if (!donor.source_readiness_topology_reused || donor.owned_duration_nodes.empty()) return {};
    const auto * fact = source.fact_node(donor.source_fact_node_id);
    if (!fact) return {};
    const auto & graph = source.graph();
    std::map<uint64_t, size_t> records;
    for (const auto & call : waits.calls) {
        if (!call.enabled || !call.issue.empty() || call.phase != "EXTEND" || call.request_id != donor.request_id
            || graph.event_for_node(*call.before).pid != fact->pid)
            continue;
        const auto [at, first] = records.emplace(call.layer, *call.record);
        if (!first && at->second != *call.record) return {}; // A new batch needs its own layout.
    }
    if (records.empty()) return {};
    std::map<size_t, uint64_t> layer_at_record;
    for (const auto & [layer, record] : records)
        if (!layer_at_record.emplace(record, layer).second) return {};
    // Some backends transfer all layers before the first Record. Later Records
    // then carry readiness, not another share of the transfer. Prove their
    // stream order and reject any work omitted from the transfer ownership.
    const std::set<size_t> transfers(donor.owned_duration_nodes.begin(), donor.owned_duration_nodes.end());
    for (auto next = std::next(records.begin()); next != records.end(); ++next) {
        auto current = std::prev(next)->second;
        std::set<size_t> visited;
        while (current != next->second) {
            if (!visited.insert(current).second) return {};
            std::optional<size_t> successor;
            for (const auto id : source.outgoing_edge_ids(current)) {
                const auto & edge = graph.edge(id);
                if (!edge.active || edge.kind != core::DagEdgeKind::Stream) continue;
                if (graph.node(edge.dst).lane_id != graph.node(current).lane_id || successor) return {};
                successor = edge.dst;
            }
            if (!successor || (*successor != next->second && !transfers.contains(*successor))) return {};
            current = *successor;
        }
    }
    std::map<uint64_t, uint64_t> bytes;
    for (const auto transfer : donor.owned_duration_nodes) {
        const auto weight = graph.event_for_node(transfer).arg_u64("size(B)");
        if (!weight) return {};
        std::vector<size_t> frontier{ transfer };
        std::set<size_t> visited;
        std::set<uint64_t> reached;
        while (!frontier.empty()) {
            const auto current = frontier.back();
            frontier.pop_back();
            if (!visited.insert(current).second) continue;
            if (const auto record = layer_at_record.find(current); record != layer_at_record.end()) {
                reached.insert(record->second);
                continue;
            }
            for (const auto id : source.outgoing_edge_ids(current)) {
                const auto & edge = graph.edge(id);
                if (edge.active && edge.kind == core::DagEdgeKind::Stream && graph.node(edge.dst).lane_id == graph.node(transfer).lane_id)
                    frontier.push_back(edge.dst);
            }
        }
        if (reached.size() != 1) return {};
        bytes[*reached.begin()] = core::checked_add_u64(bytes[*reached.begin()], weight, "layer transfer bytes overflow");
    }
    std::vector<HiCacheLayerTransferTemplate> result;
    for (const auto & [layer, record] : records) { result.push_back({ layer, bytes[layer], graph.node(record).original_duration, record }); }
    return result;
}

bool expand_hicache_layer_io(const std::vector<HiCacheLayerTransferTemplate> & layers, HiCacheRewriteDecision & decision, core::DagMutationPlan & plan) {
    using Ref = core::DagNodeRef;
    if (layers.empty() || decision.source_readiness_topology_reused || !decision.layer_io.empty() || decision.effect_type != model::HiCacheEffectType::Loadback
        || !decision.source_execution_anchor_node_id)
        return false;
    const auto aggregate = std::ranges::find(plan.synthetic_nodes, decision.synthetic_id, &core::DagSyntheticNodeMutation::synthetic_id);
    if (aggregate == plan.synthetic_nodes.end() || aggregate->node.duration != decision.duration_us) return false;
    uint64_t total_bytes = 0;
    std::set<uint64_t> seen;
    for (const auto & layer : layers) {
        if (!seen.insert(layer.layer).second) return false;
        total_bytes = core::checked_add_u64(total_bytes, layer.bytes, "layer template bytes overflow");
    }
    if (!total_bytes) return false;
    aggregate->node.duration = 0;
    uint64_t cumulative_bytes = 0, assigned = 0;
    for (const auto & layer : layers) {
        cumulative_bytes += layer.bytes;
        const auto cumulative = core::floor_multiply_divide_u64(decision.duration_us, cumulative_bytes, total_bytes);
        if (!cumulative) throw std::overflow_error("layer service projection overflow");
        const auto prefix = decision.synthetic_id + ":layer:" + std::to_string(layer.layer);
        decision.layer_io.push_back({ .layer = layer.layer,
                                      .service_id = prefix + ":transfer",
                                      .ready_id = prefix + ":record",
                                      .service_us = *cumulative - assigned,
                                      .record_us = layer.record_us });
        assigned = *cumulative;
    }
    const auto ingress = decision.target_host_control_required ? Ref::synthetic(decision.target_host_control_synthetic_id)
                                                               : Ref::existing(*decision.source_execution_anchor_node_id);
    const auto finish = Ref::synthetic(decision.layer_io.back().ready_id);
    // Keep CPU submission/control readiness, not an entire-device-service barrier.
    // Other egresses (especially serialized I/O) retain the full operation end.
    for (auto & edge : plan.add_edges) {
        if (edge.src.synthetic_id != decision.synthetic_id) continue;
        const bool foreground =
            edge.effect_id == decision.effect_id
            && ((!decision.completion_join_synthetic_id.empty() && edge.dst.synthetic_id == decision.completion_join_synthetic_id)
                || (edge.dst.existing_node_id && std::ranges::find(decision.consumer_anchors, *edge.dst.existing_node_id) != decision.consumer_anchors.end()));
        edge.src = foreground ? ingress : finish;
    }
    // A control branch may already be present in the aggregate transaction.
    std::set<std::tuple<std::optional<size_t>, std::string, std::optional<size_t>, std::string, core::DagEdgeKind, std::string>> unique;
    std::erase_if(plan.add_edges, [&](const auto & edge) {
        if (edge.effect_id != decision.effect_id) return false;
        return !unique.emplace(edge.src.existing_node_id, edge.src.synthetic_id, edge.dst.existing_node_id, edge.dst.synthetic_id, edge.kind, edge.effect_id)
                    .second;
    });
    decision.completion_join_uses_service = false;
    Ref previous = Ref::synthetic(decision.synthetic_id);
    for (size_t i = 0; i < decision.layer_io.size(); ++i) {
        const auto & layer = decision.layer_io[i];
        for (const auto & [id, duration] : {
                 std::pair{ layer.service_id, layer.service_us },
                 std::pair{   layer.ready_id,  layer.record_us }
        }) {
            plan.synthetic_nodes.push_back({
                .synthetic_id = id,
                .node = { .name = id,
                         .category = "hicache_patch",
                         .is_cpu = false,
                         .lane_key = decision.resource_lane,
                         .duration = duration,
                         .attrs = { { "request_id", decision.request_id }, { "source_record", std::to_string(layers[i].source_record) } } },
                .effect_id = decision.effect_id,
                .reason = "base-observed layer layout; predicted transfer service is conserved"
            });
            plan.add_edges.push_back({ .src = previous,
                                       .dst = Ref::synthetic(id),
                                       .kind = core::DagEdgeKind::Mutation,
                                       .effect_id = decision.effect_id,
                                       .reason = "ordered transfer and per-layer readiness" });
            previous = Ref::synthetic(id);
        }
    }
    return true;
}
} // namespace markov::trace_graph::modules::hicache::patch
