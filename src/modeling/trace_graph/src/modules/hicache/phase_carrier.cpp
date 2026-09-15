/**
 * @file
 * @brief Canonical request/rank Prefill and Decode carrier construction.
 */
#include "markov/trace_graph/modules/hicache/phase_carrier.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache {

std::string hicache_phase_carrier_synthetic_id(std::string_view request_id,
                                               int logical_input,
                                               std::string_view phase,
                                               std::string_view family) {
    return "hicache_phase_carrier:hicache_phase:" + std::string(request_id) + ":" + std::string(phase) + ":"
           + std::to_string(logical_input) + ":" + std::string(family);
}

namespace {

using model::HiCacheDecodeWorkItem;
using model::HiCachePhaseNodeCostPlan;
using model::HiCachePrefillWorkItem;

void add_blocker(HiCachePhaseCarrierAudit & audit, std::string blocker) {
    (void)core::checked_increment_u64(audit.blockers[std::move(blocker)], "HiCache phase-carrier blocker count exceeds uint64 range");
}

std::string phase_effect(std::string_view request_id, int logical_input, std::string_view phase, std::string_view family) {
    return "hicache_phase:" + std::string(request_id) + ":" + std::string(phase) + ":" + std::to_string(logical_input) + ":"
           + std::string(family);
}

std::string carrier_id(std::string_view effect_id) { return "hicache_phase_carrier:" + std::string(effect_id); }

std::optional<size_t> ordered_node(const core::DagGraph & graph, const std::vector<size_t> & nodes, bool latest) {
    std::optional<size_t> result;
    for (const auto node_id : nodes) {
        if (node_id >= graph.node_count() || !graph.node(node_id).active) return std::nullopt;
        if (!result) {
            result = node_id;
            continue;
        }
        const auto & event = graph.event_for_node(node_id);
        const auto & selected = graph.event_for_node(*result);
        const auto event_key = std::pair{ event.ts, node_id };
        const auto selected_key = std::pair{ selected.ts, *result };
        if ((!latest && event_key < selected_key) || (latest && event_key > selected_key)) result = node_id;
    }
    return result;
}

bool validate_source_family(const core::DagGraph & graph, const HiCachePhaseNodeCostPlan & cost, std::set<size_t> & owned_nodes,
                            HiCachePhaseCarrierAudit & audit) {
    uint64_t actual = 0;
    for (const auto node_id : cost.source_node_ids) {
        if (node_id >= graph.node_count() || !graph.node(node_id).active) {
            add_blocker(audit, "source_device_node_invalid");
            return false;
        }
        if (!owned_nodes.insert(node_id).second) {
            ++audit.owner_conflict_count;
            add_blocker(audit, "source_device_owner_conflict");
            return false;
        }
        actual = core::checked_add_u64(actual, graph.node(node_id).duration, "HiCache phase source duration exceeds uint64 range");
    }
    if (actual != cost.source_duration_us) {
        add_blocker(audit, "source_device_duration_mismatch");
        return false;
    }
    return true;
}

void zero_source_family(const HiCachePhaseNodeCostPlan & cost, std::string_view effect_id, core::DagMutationPlan & plan,
                        HiCachePhaseCarrierAudit & audit) {
    for (const auto node_id : cost.source_node_ids) {
        plan.set_node_durations.push_back(core::DagSetNodeDurationMutation{
            .node_id = node_id,
            .duration = 0,
            .effect_id = std::string(effect_id),
            .reason = "move phase cost from source-specific device topology to one semantic carrier",
        });
        plan.set_node_e2e_eligibility.push_back(core::DagSetNodeE2eEligibilityMutation{
            .node_id = node_id,
            .counts_toward_e2e = false,
            .effect_id = std::string(effect_id),
            .reason = "zero-cost source phase skeleton is not a standalone business endpoint",
        });
        ++audit.source_device_node_count;
    }
}

bool project_submit_cost(const core::DagGraph & graph, const HiCachePhaseNodeCostPlan & cost, std::string_view effect_id,
                         core::DagMutationPlan & plan, std::set<size_t> & owned_nodes, HiCachePhaseCarrierAudit & audit) {
    if (cost.source_node_ids.empty() || cost.source_duration_us == 0) {
        add_blocker(audit, "phase_submit_carrier_missing");
        return false;
    }
    uint64_t actual = 0;
    for (const auto node_id : cost.source_node_ids) {
        if (node_id >= graph.node_count() || !graph.node(node_id).active || !graph.node(node_id).is_cpu
            || !owned_nodes.insert(node_id).second) {
            ++audit.owner_conflict_count;
            add_blocker(audit, "phase_submit_owner_invalid");
            return false;
        }
        actual = core::checked_add_u64(actual, graph.node(node_id).duration, "HiCache phase submit duration exceeds uint64 range");
    }
    if (actual != cost.source_duration_us) {
        add_blocker(audit, "phase_submit_duration_mismatch");
        return false;
    }
    uint64_t assigned = 0;
    for (size_t index = 0; index < cost.source_node_ids.size(); ++index) {
        const auto node_id = cost.source_node_ids[index];
        uint64_t duration = 0;
        if (index + 1 == cost.source_node_ids.size()) duration = cost.predicted_duration_us - assigned;
        else {
            const auto projected = core::floor_multiply_divide_u64(graph.node(node_id).duration,
                                                                   cost.predicted_duration_us,
                                                                   cost.source_duration_us);
            if (!projected) {
                add_blocker(audit, "phase_submit_projection_overflow");
                return false;
            }
            duration = *projected;
            assigned = core::checked_add_u64(assigned, duration, "HiCache projected phase submit duration exceeds uint64 range");
        }
        plan.set_node_durations.push_back(core::DagSetNodeDurationMutation{
            .node_id = node_id,
            .duration = duration,
            .effect_id = std::string(effect_id),
            .reason = "project active phase submit cost on its correlated source CPU carriers",
        });
        ++audit.source_submit_node_count;
    }
    return true;
}

struct CarrierRecord {
    const HiCachePrefillWorkItem * prefill = nullptr;
    const HiCacheDecodeWorkItem * decode = nullptr;
    size_t first_prefill_submit = 0;
    size_t last_prefill_submit = 0;
    size_t first_decode_submit = 0;
    size_t last_decode_submit = 0;
    uint64_t order_ts = 0;
    std::string prefill_common;
    std::string prefill_prefix;
    std::string prefill_collective;
    std::string decode_kernel;
    std::string decode_collective;
};

void append_carrier_node(core::DagMutationPlan & plan, std::string_view synthetic_id, std::string_view effect_id,
                         std::string_view request_id, int logical_input, std::string_view phase, std::string_view family,
                         uint64_t duration_us, bool endpoint, HiCachePhaseCarrierAudit & audit) {
    plan.synthetic_nodes.push_back(core::DagSyntheticNodeMutation{
        .synthetic_id = std::string(synthetic_id),
        .node = core::DagSyntheticNodeSpec{
            .name = "hicache_phase_" + std::string(phase) + "_" + std::string(family),
            .category = "hicache_phase",
            .is_cpu = false,
            .lane_key = "scope:" + std::to_string(logical_input + 1) + "/phase_device_lane",
            .duration = duration_us,
            .counts_toward_e2e = endpoint,
            .attrs = {
                { "request_id", std::string(request_id) },
                { "phase", std::string(phase) },
                { "family", std::string(family) },
                { "logical_input", std::to_string(logical_input) },
            },
        },
        .effect_id = std::string(effect_id),
        .reason = "materialize source-independent request/rank phase work",
    });
    ++audit.synthetic_carrier_count;
}

void append_edge(core::DagMutationPlan & plan, core::DagNodeRef src, core::DagNodeRef dst, std::string effect_id,
                 std::string reason, HiCachePhaseCarrierAudit & audit) {
    plan.add_edges.push_back(core::DagAddEdgeMutation{
        .src = std::move(src),
        .dst = std::move(dst),
        .kind = core::DagEdgeKind::Mutation,
        .effect_id = std::move(effect_id),
        .reason = std::move(reason),
    });
    ++audit.dependency_count;
}

bool append_record(const core::DagGraph & graph, const HiCachePrefillWorkItem & prefill, const HiCacheDecodeWorkItem & decode,
                   core::DagMutationPlan & plan, std::set<size_t> & owned_nodes, HiCachePhaseCarrierAudit & audit,
                   CarrierRecord & record) {
    const auto first_prefill = ordered_node(graph, prefill.submit_cost.source_node_ids, false);
    const auto last_prefill = ordered_node(graph, prefill.submit_cost.source_node_ids, true);
    const auto first_decode = ordered_node(graph, decode.submit_cost.source_node_ids, false);
    const auto last_decode = ordered_node(graph, decode.submit_cost.source_node_ids, true);
    if (!first_prefill || !last_prefill || !first_decode || !last_decode) {
        add_blocker(audit, "phase_submit_boundary_missing");
        return false;
    }
    const auto common_effect = phase_effect(prefill.request_id, prefill.logical_input, "prefill", "common_kernel");
    const auto prefix_effect = phase_effect(prefill.request_id, prefill.logical_input, "prefill", "prefix_attention");
    const auto prefill_collective_effect = phase_effect(prefill.request_id, prefill.logical_input, "prefill", "collective");
    const auto prefill_submit_effect = phase_effect(prefill.request_id, prefill.logical_input, "prefill", "submit");
    const auto decode_kernel_effect = phase_effect(decode.request_id, decode.logical_input, "decode", "kernel");
    const auto decode_collective_effect = phase_effect(decode.request_id, decode.logical_input, "decode", "collective");
    const auto decode_submit_effect = phase_effect(decode.request_id, decode.logical_input, "decode", "submit");

    bool ready = true;
    for (const auto * cost : { &prefill.common_kernel_cost, &prefill.prefix_attention_cost, &prefill.collective_cost,
                              &decode.kernel_cost, &decode.collective_cost }) {
        ready = validate_source_family(graph, *cost, owned_nodes, audit) && ready;
    }
    ready = project_submit_cost(graph, prefill.submit_cost, prefill_submit_effect, plan, owned_nodes, audit) && ready;
    ready = project_submit_cost(graph, decode.submit_cost, decode_submit_effect, plan, owned_nodes, audit) && ready;
    if (!ready) return false;

    zero_source_family(prefill.common_kernel_cost, common_effect, plan, audit);
    zero_source_family(prefill.prefix_attention_cost, prefix_effect, plan, audit);
    zero_source_family(prefill.collective_cost, prefill_collective_effect, plan, audit);
    zero_source_family(decode.kernel_cost, decode_kernel_effect, plan, audit);
    zero_source_family(decode.collective_cost, decode_collective_effect, plan, audit);

    record = CarrierRecord{
        .prefill = &prefill,
        .decode = &decode,
        .first_prefill_submit = *first_prefill,
        .last_prefill_submit = *last_prefill,
        .first_decode_submit = *first_decode,
        .last_decode_submit = *last_decode,
        .order_ts = graph.event_for_node(*first_prefill).ts,
        .prefill_common = carrier_id(common_effect),
        .prefill_prefix = carrier_id(prefix_effect),
        .prefill_collective = carrier_id(prefill_collective_effect),
        .decode_kernel = carrier_id(decode_kernel_effect),
        .decode_collective = carrier_id(decode_collective_effect),
    };
    append_carrier_node(plan,
                        record.prefill_common,
                        common_effect,
                        prefill.request_id,
                        prefill.logical_input,
                        "prefill",
                        "common_kernel",
                        prefill.common_kernel_cost.predicted_duration_us,
                        false,
                        audit);
    append_carrier_node(plan,
                        record.prefill_prefix,
                        prefix_effect,
                        prefill.request_id,
                        prefill.logical_input,
                        "prefill",
                        "prefix_attention",
                        prefill.prefix_attention_cost.predicted_duration_us,
                        false,
                        audit);
    append_carrier_node(plan,
                        record.prefill_collective,
                        prefill_collective_effect,
                        prefill.request_id,
                        prefill.logical_input,
                        "prefill",
                        "collective",
                        prefill.collective_cost.predicted_duration_us,
                        false,
                        audit);
    append_carrier_node(plan,
                        record.decode_kernel,
                        decode_kernel_effect,
                        decode.request_id,
                        decode.logical_input,
                        "decode",
                        "kernel",
                        decode.kernel_cost.predicted_duration_us,
                        false,
                        audit);
    append_carrier_node(plan,
                        record.decode_collective,
                        decode_collective_effect,
                        decode.request_id,
                        decode.logical_input,
                        "decode",
                        "collective",
                        decode.collective_cost.predicted_duration_us,
                        true,
                        audit);

    const auto topology_effect = "hicache_phase_topology:" + prefill.request_id + ":" + std::to_string(prefill.logical_input);
    append_edge(plan,
                core::DagNodeRef::existing(record.first_prefill_submit),
                core::DagNodeRef::synthetic(record.prefill_common),
                topology_effect + ":prefill_ingress",
                "active Prefill submission makes common device work eligible",
                audit);
    append_edge(plan,
                core::DagNodeRef::synthetic(record.prefill_common),
                core::DagNodeRef::synthetic(record.prefill_prefix),
                topology_effect + ":prefill_common_prefix",
                "Prefill prefix work follows common-kernel work",
                audit);
    append_edge(plan,
                core::DagNodeRef::synthetic(record.prefill_prefix),
                core::DagNodeRef::synthetic(record.prefill_collective),
                topology_effect + ":prefill_prefix_collective",
                "Prefill collective follows the optional prefix family",
                audit);
    append_edge(plan,
                core::DagNodeRef::existing(record.last_prefill_submit),
                core::DagNodeRef::synthetic(record.prefill_collective),
                topology_effect + ":prefill_submit_join",
                "Prefill completion waits for active submission control",
                audit);
    append_edge(plan,
                core::DagNodeRef::synthetic(record.prefill_collective),
                core::DagNodeRef::existing(record.first_decode_submit),
                topology_effect + ":prefill_decode",
                "Decode submission follows Prefill completion",
                audit);
    append_edge(plan,
                core::DagNodeRef::existing(record.first_decode_submit),
                core::DagNodeRef::synthetic(record.decode_kernel),
                topology_effect + ":decode_ingress",
                "active Decode submission makes device work eligible",
                audit);
    append_edge(plan,
                core::DagNodeRef::synthetic(record.decode_kernel),
                core::DagNodeRef::synthetic(record.decode_collective),
                topology_effect + ":decode_collective",
                "Decode collective follows Decode kernel work",
                audit);
    append_edge(plan,
                core::DagNodeRef::existing(record.last_decode_submit),
                core::DagNodeRef::synthetic(record.decode_collective),
                topology_effect + ":decode_submit_join",
                "Decode completion waits for active submission control",
                audit);
    return true;
}

} // namespace

HiCachePhaseCarrierAudit append_hicache_phase_carrier_plan(const core::DagGraph & graph,
                                                            const model::HiCachePhaseWorkLedger & phase_work,
                                                            core::DagMutationPlan & plan) {
    HiCachePhaseCarrierAudit audit;
    if (phase_work.status != "ready" || phase_work.cost_status != "ready") {
        add_blocker(audit, "phase_work_or_cost_not_ready");
        audit.status = "blocked";
        return audit;
    }
    using RequestKey = std::pair<std::string, std::string>;
    std::map<RequestKey, const HiCacheDecodeWorkItem *> decodes;
    for (const auto & decode : phase_work.decodes) {
        if (!decodes.emplace(RequestKey{ decode.pid, decode.request_id }, &decode).second) add_blocker(audit, "duplicate_decode_request_rank");
    }
    std::set<size_t> owned_nodes;
    for (const auto & update : plan.set_node_durations) owned_nodes.insert(update.node_id);
    std::vector<CarrierRecord> records;
    records.reserve(phase_work.prefills.size());
    std::set<RequestKey> prefills;
    for (const auto & prefill : phase_work.prefills) {
        const RequestKey key{ prefill.pid, prefill.request_id };
        if (!prefills.insert(key).second) {
            add_blocker(audit, "duplicate_prefill_request_rank");
            continue;
        }
        const auto decode = decodes.find(key);
        if (decode == decodes.end() || decode->second->logical_input != prefill.logical_input) {
            add_blocker(audit, "decode_request_rank_missing");
            continue;
        }
        CarrierRecord record;
        if (append_record(graph, prefill, *decode->second, plan, owned_nodes, audit, record)) records.push_back(std::move(record));
    }
    audit.request_rank_count = records.size();

    std::ranges::sort(records, [](const auto & left, const auto & right) {
        if (left.order_ts != right.order_ts) return left.order_ts < right.order_ts;
        if (left.prefill->request_id != right.prefill->request_id) return left.prefill->request_id < right.prefill->request_id;
        return left.prefill->logical_input < right.prefill->logical_input;
    });
    std::vector<std::vector<const CarrierRecord *>> request_groups;
    for (const auto & record : records) {
        auto group = std::ranges::find_if(request_groups, [&](const auto & candidate) {
            return !candidate.empty() && candidate.front()->prefill->request_id == record.prefill->request_id;
        });
        if (group == request_groups.end()) request_groups.push_back({ &record });
        else group->push_back(&record);
    }
    std::ranges::sort(request_groups, [](const auto & left, const auto & right) {
        return std::ranges::min(left, {}, &CarrierRecord::order_ts)->order_ts
               < std::ranges::min(right, {}, &CarrierRecord::order_ts)->order_ts;
    });
    audit.request_count = request_groups.size();
    for (size_t index = 1; index < request_groups.size(); ++index) {
        const auto & previous = request_groups[index - 1];
        const auto & current = request_groups[index];
        for (const auto * before : previous) {
            for (const auto * after : current) {
                append_edge(plan,
                            core::DagNodeRef::synthetic(before->decode_collective),
                            core::DagNodeRef::existing(after->first_prefill_submit),
                            "hicache_phase_request_boundary:" + before->prefill->request_id + "->" + after->prefill->request_id + ":"
                                + std::to_string(before->prefill->logical_input) + ":" + std::to_string(after->prefill->logical_input),
                            "the next formal request starts after every prior rank completes Decode",
                            audit);
            }
        }
    }
    if (records.size() != phase_work.prefills.size() || records.size() != phase_work.decodes.size()) add_blocker(audit, "request_rank_coverage_incomplete");
    audit.status = audit.blockers.empty() && audit.owner_conflict_count == 0 ? "ready" : "blocked";
    return audit;
}

} // namespace markov::trace_graph::modules::hicache
