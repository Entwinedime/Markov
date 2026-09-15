/**
 * @file
 * @brief Source-supported phase operators with zero-cost request boundaries.
 */
#include "markov/trace_graph/modules/hicache/phase_carrier.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"

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

bool project_cost(const core::DagGraph & graph, const HiCachePhaseNodeCostPlan & cost, std::string_view effect_id, bool cpu,
                  core::DagMutationPlan & plan, std::set<size_t> & owned_nodes, HiCachePhaseCarrierAudit & audit) {
    if (cost.source_duration_us == 0 && cost.predicted_duration_us != 0) {
        add_blocker(audit, "phase_operator_template_missing");
        return false;
    }
    uint64_t actual = 0;
    for (const auto node_id : cost.source_node_ids) {
        if (node_id >= graph.node_count() || !graph.node(node_id).active || graph.node(node_id).is_cpu != cpu
            || !owned_nodes.insert(node_id).second) {
            ++audit.owner_conflict_count;
            add_blocker(audit, "phase_source_owner_invalid");
            return false;
        }
        actual = core::checked_add_u64(actual, graph.node(node_id).duration, "HiCache phase source duration exceeds uint64 range");
    }
    if (actual != cost.source_duration_us) {
        add_blocker(audit, "phase_source_duration_mismatch");
        return false;
    }
    uint64_t cumulative = 0, assigned = 0;
    for (const auto node_id : cost.source_node_ids) {
        cumulative += graph.node(node_id).duration; // Bounded by the checked total.
        const auto projected = cost.source_duration_us
            ? core::floor_multiply_divide_u64(cumulative, cost.predicted_duration_us, cost.source_duration_us)
            : std::optional<uint64_t>{0};
        if (!projected) {
            add_blocker(audit, "phase_projection_overflow");
            return false;
        }
        plan.set_node_durations.push_back(core::DagSetNodeDurationMutation{
            .node_id = node_id,
            .duration = *projected - assigned,
            .effect_id = std::string(effect_id),
            .reason = "allocate target family cost by measured source operator shares; preserve dependencies",
        });
        assigned = *projected;
        if (cpu) ++audit.source_submit_node_count;
        else ++audit.source_device_node_count;
    }
    return true;
}

bool project_decode_kernels(const core::DagGraph& graph, const HiCacheDecodeWorkItem& decode,
                            core::DagMutationPlan& plan, std::set<size_t>& owned_nodes, HiCachePhaseCarrierAudit& audit) {
    const auto& total = decode.kernel_cost;
    if (decode.source_paged_attention_duration_us > total.source_duration_us
        || decode.predicted_paged_attention_duration_us > total.predicted_duration_us) {
        add_blocker(audit, "decode_attention_exceeds_kernel_cost");
        return false;
    }
    HiCachePhaseNodeCostPlan attention{decode.source_paged_attention_duration_us, decode.predicted_paged_attention_duration_us, {}};
    HiCachePhaseNodeCostPlan common{total.source_duration_us - attention.source_duration_us,
                                   total.predicted_duration_us - attention.predicted_duration_us, {}};
    for (const auto id : total.source_node_ids) {
        if (id >= graph.node_count()) {
            add_blocker(audit, "phase_source_owner_invalid");
            return false;
        }
        auto& family = is_hicache_paged_attention(graph.event_for_node(id).name) ? attention : common;
        family.source_node_ids.push_back(id);
    }
    // Keep the existing aggregate effect identity, but never spread attention
    // changes onto unrelated kernels. Oracle costs supply both components too.
    const auto effect = phase_effect(decode.request_id, decode.logical_input, "decode", "kernel");
    const bool common_ready = project_cost(graph, common, effect, false, plan, owned_nodes, audit);
    return project_cost(graph, attention, effect, false, plan, owned_nodes, audit) && common_ready;
}

struct CarrierRecord {
    const HiCachePrefillWorkItem * prefill = nullptr;
    uint64_t order_ts = 0;
    std::string prefill_start;
    std::string prefill_complete;
    std::string decode_start;
    std::string decode_complete;
};

void append_boundary_node(core::DagMutationPlan & plan, std::string_view synthetic_id, std::string_view effect_id,
                         std::string_view request_id, int logical_input, std::string_view phase, std::string_view family,
                         bool endpoint, HiCachePhaseCarrierAudit & audit) {
    plan.synthetic_nodes.push_back(core::DagSyntheticNodeMutation{
        .synthetic_id = std::string(synthetic_id),
        .node = core::DagSyntheticNodeSpec{
            .name = "hicache_phase_" + std::string(phase) + "_" + std::string(family),
            .category = "hicache_phase",
            .is_cpu = false,
            .lane_key = "scope:" + std::to_string(logical_input + 1) + "/phase_boundary",
            .duration = 0,
            .counts_toward_e2e = endpoint,
            .attrs = {
                { "request_id", std::string(request_id) },
                { "phase", std::string(phase) },
                { "family", std::string(family) },
                { "logical_input", std::to_string(logical_input) },
            },
        },
        .effect_id = std::string(effect_id),
        .reason = "zero-cost phase boundary; operators retain submission and synchronization",
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
    const auto project = [&](const HiCachePhaseNodeCostPlan& cost, std::string_view phase, std::string_view family, bool cpu = false) {
        return project_cost(graph, cost, phase_effect(prefill.request_id, prefill.logical_input, phase, family),
                            cpu, plan, owned_nodes, audit);
    };
    bool ready = project(prefill.submit_cost, "prefill", "submit", true);
    ready = project(decode.submit_cost, "decode", "submit", true) && ready;
    ready = project(prefill.common_kernel_cost, "prefill", "common_kernel") && ready;
    ready = project(prefill.prefix_attention_cost, "prefill", "prefix_attention") && ready;
    ready = project(prefill.collective_cost, "prefill", "collective") && ready;
    ready = project_decode_kernels(graph, decode, plan, owned_nodes, audit) && ready;
    ready = project(decode.collective_cost, "decode", "collective") && ready;
    if (!ready) return false;

    record = CarrierRecord{
        .prefill = &prefill,
        .order_ts = graph.event_for_node(*first_prefill).ts,
        .prefill_start = carrier_id(phase_effect(prefill.request_id, prefill.logical_input, "prefill", "start")),
        .prefill_complete = carrier_id(phase_effect(prefill.request_id, prefill.logical_input, "prefill", "complete")),
        .decode_start = carrier_id(phase_effect(decode.request_id, decode.logical_input, "decode", "start")),
        .decode_complete = carrier_id(phase_effect(decode.request_id, decode.logical_input, "decode", "complete")),
    };
    const auto boundaries = [&](std::string_view phase, const std::string& start, const std::string& complete,
                                size_t first, size_t last,
                                std::initializer_list<const HiCachePhaseNodeCostPlan*> families) {
        const auto boundary_effect = phase_effect(prefill.request_id, prefill.logical_input, phase, "boundary");
        append_boundary_node(plan, start, boundary_effect, prefill.request_id, prefill.logical_input, phase, "start", false, audit);
        append_boundary_node(plan, complete, boundary_effect, prefill.request_id, prefill.logical_input, phase, "complete", true, audit);
        const auto connect = [&](core::DagNodeRef from, core::DagNodeRef to) {
            append_edge(plan, std::move(from), std::move(to), boundary_effect,
                        "phase boundary connects work without relocating its execution cost", audit);
        };
        connect(core::DagNodeRef::existing(first), core::DagNodeRef::synthetic(start));
        connect(core::DagNodeRef::synthetic(start), core::DagNodeRef::synthetic(complete));
        connect(core::DagNodeRef::existing(last), core::DagNodeRef::synthetic(complete));
        for (const auto* cost : families) for (const auto id : cost->source_node_ids) {
            connect(core::DagNodeRef::synthetic(start), core::DagNodeRef::existing(id));
            connect(core::DagNodeRef::existing(id), core::DagNodeRef::synthetic(complete));
        }
    };
    boundaries("prefill", record.prefill_start, record.prefill_complete, *first_prefill, *last_prefill,
               {&prefill.common_kernel_cost, &prefill.prefix_attention_cost, &prefill.collective_cost});
    boundaries("decode", record.decode_start, record.decode_complete, *first_decode, *last_decode,
               {&decode.kernel_cost, &decode.collective_cost});
    // CPU preparation may overlap prior device work. Original stream and sync
    // edges, not a whole-Prefill-to-first-Decode-submit edge, constrain execution.
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
                            core::DagNodeRef::synthetic(before->decode_complete),
                            core::DagNodeRef::synthetic(after->prefill_start),
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
