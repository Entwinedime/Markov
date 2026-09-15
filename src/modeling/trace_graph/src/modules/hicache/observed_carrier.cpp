/**
 * @file
 * @brief Score-side canonical Direct and phase carriers for target observations.
 */
#include "markov/trace_graph/modules/hicache/phase_carrier.hpp"

#ifdef DEBUG

#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_operation_ledger.hpp"
#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache {

namespace {

using DirectAudit = HiCacheObservedPhaseCarrierResult::DirectCarrierAudit;
using patch::HiCacheCpuGapSlice;
using patch::HiCacheIoOperationKind;
using patch::HiCacheIoOperationLedger;
using patch::HiCacheIoOperationRecord;

model::HiCachePhaseWorkLedger observed_phase_work(const HiCachePhaseObservationAudit & observed) {
    model::HiCachePhaseWorkLedger work;
    for (const auto & row : observed.observations) {
        if (row.request_ids.size() != 1) continue;
        const auto cost = [](uint64_t duration, const std::vector<size_t> & nodes) {
            return model::HiCachePhaseNodeCostPlan{
                .source_duration_us = duration,
                .predicted_duration_us = duration,
                .source_node_ids = nodes,
            };
        };
        work.prefills.push_back(model::HiCachePrefillWorkItem{
            .logical_input = row.logical_input,
            .pid = row.pid,
            .request_id = row.request_ids.front(),
            .batch_size = row.batch_size,
            .prompt_token_count = row.prompt_token_count,
            .prefill_token_count = row.prefill_token_count,
            .source_prefill_token_count = row.prefill_token_count,
            .common_kernel_cost = cost(row.prefill_common_kernel_duration_us, row.prefill_common_kernel_node_ids),
            .prefix_attention_cost = cost(row.prefill_prefix_attention_duration_us, row.prefill_prefix_attention_node_ids),
            .kernel_cost = cost(row.prefill_kernel_duration_us, row.prefill_kernel_node_ids),
            .collective_cost = cost(row.prefill_collective_duration_us, row.prefill_collective_node_ids),
            .submit_cost = cost(row.prefill_submit_cpu_duration_us, row.prefill_submit_cpu_node_ids),
            .feature_covered = true,
        });
        work.decodes.push_back(model::HiCacheDecodeWorkItem{
            .logical_input = row.logical_input,
            .pid = row.pid,
            .request_id = row.request_ids.front(),
            .prompt_token_count = row.prompt_token_count,
            .target_page_size = row.source_page_size,
            .iteration_count = row.decode_iteration_count,
            .source_paged_attention_duration_us = hicache_paged_attention_duration(row.decode_kernel_families),
            .predicted_paged_attention_duration_us = hicache_paged_attention_duration(row.decode_kernel_families),
            .kernel_cost = cost(row.decode_kernel_duration_us, row.decode_kernel_node_ids),
            .collective_cost = cost(row.decode_collective_duration_us, row.decode_collective_node_ids),
            .submit_cost = cost(row.decode_submit_cpu_duration_us, row.decode_submit_cpu_node_ids),
            .feature_covered = true,
        });
    }
    work.prefill_status = work.prefills.size() == observed.observations.size() ? "ready" : "not_ready";
    work.decode_status = work.decodes.size() == work.prefills.size() ? "ready" : "not_ready";
    work.cost_status = work.prefill_status == "ready" && work.decode_status == "ready" ? "ready" : "not_ready";
    work.status = observed.ready() && work.cost_status == "ready" ? "ready" : "not_ready";
    return work;
}

void add_blocker(DirectAudit & audit, std::string blocker) {
    (void)core::checked_increment_u64(audit.blockers[std::move(blocker)], "Observed Direct-carrier blocker count exceeds uint64 range");
}

std::optional<size_t> unique_sequential_ingress(const core::DagGraph & graph, size_t node_id) {
    std::optional<size_t> match;
    for (size_t edge_id = 0; edge_id < graph.edge_count(); ++edge_id) {
        const auto & edge = graph.edge(edge_id);
        if (!edge.active || edge.dst != node_id || edge.kind != core::DagEdgeKind::Sequential) continue;
        if (match) return std::nullopt;
        match = edge_id;
    }
    return match;
}

bool canonical_prefetch(const HiCacheIoOperationRecord & operation) {
    return operation.kind == HiCacheIoOperationKind::Prefetch && patch::hicache_io_operation_record_ready(operation) && operation.completed_token_count > 0;
}

struct CanonicalPrefetch {
    const HiCacheIoOperationRecord * operation = nullptr;
    std::string service_id;
    std::string join_id;
};

void append_edge(core::DagMutationPlan & plan, core::DagNodeRef src, core::DagNodeRef dst, std::string effect_id, std::string reason, DirectAudit & audit) {
    plan.add_edges.push_back(core::DagAddEdgeMutation{
        .src = std::move(src),
        .dst = std::move(dst),
        .kind = core::DagEdgeKind::Mutation,
        .effect_id = std::move(effect_id),
        .reason = std::move(reason),
    });
    ++audit.dependency_count;
}

bool append_prefetch(const core::DagGraph & graph, const HiCacheIoOperationRecord & operation, core::DagMutationPlan & plan, DirectAudit & audit,
                     CanonicalPrefetch & output) {
    if (!operation.source_anchor_node_id || !operation.control_ready_anchor_node_id || !operation.wait_exit_anchor_node_id
        || !operation.terminal_control_anchor_node_id || !operation.completion_join_contract_ready || operation.storage_service_batches.empty()
        || operation.observed_service_duration_us == 0 || operation.request_id.empty() || operation.cache_scope.empty()) {
        add_blocker(audit, "prefetch_completion_contract_incomplete");
        return false;
    }
    const auto service_duration_us = operation.observed_service_duration_us;
    const auto service = "hicache_observed_prefetch_service:" + operation.record_id;
    const auto join = "hicache_observed_prefetch_join:" + operation.record_id;
    plan.synthetic_nodes.push_back(core::DagSyntheticNodeMutation{
        .synthetic_id = service,
        .node = core::DagSyntheticNodeSpec{
            .name = "hicache_observed_prefetch_service",
            .category = "hicache_patch",
            .is_cpu = false,
            .lane_key = operation.cache_scope + "/host_storage_read_lane",
            .duration = service_duration_us,
            .counts_toward_e2e = false,
            .attrs = {
                { "record_id", operation.record_id },
                { "request_id", operation.request_id },
                { "cost_model", "target_observed_service" },
            },
        },
        .effect_id = operation.record_id,
        .reason = "represent target-observed Prefetch with the same full-service carrier used by prediction",
    });
    plan.synthetic_nodes.push_back(core::DagSyntheticNodeMutation{
        .synthetic_id = join,
        .node = core::DagSyntheticNodeSpec{
            .name = "hicache_observed_prefetch_completion_join",
            .category = "hicache_patch",
            .is_cpu = false,
            .lane_key = "hicache_completion_join",
            .duration = 0,
            .counts_toward_e2e = false,
            .attrs = {
                { "record_id", operation.record_id },
                { "request_id", operation.request_id },
                { "join_semantics", "max_control_ready_io_complete" },
            },
        },
        .effect_id = operation.record_id,
        .reason = "join target-observed control readiness with full Prefetch service completion",
    });
    audit.synthetic_node_count += 2;
    append_edge(plan,
                core::DagNodeRef::existing(*operation.source_anchor_node_id),
                core::DagNodeRef::synthetic(service),
                operation.record_id,
                "target-observed Prefetch starts at its executable source opportunity",
                audit);
    append_edge(plan,
                core::DagNodeRef::synthetic(service),
                core::DagNodeRef::synthetic(join),
                operation.record_id,
                "full target-observed service supplies the I/O completion branch",
                audit);
    if (*operation.control_ready_anchor_node_id == *operation.wait_exit_anchor_node_id) {
        const auto ingress = unique_sequential_ingress(graph, *operation.control_ready_anchor_node_id);
        if (!ingress) {
            add_blocker(audit, "prefetch_immediate_ready_ingress_ambiguous");
            return false;
        }
        plan.redirect_edges.push_back(core::DagRedirectEdgeMutation{
            .edge_index = *ingress,
            .dst = core::DagNodeRef::synthetic(join),
            .effect_id = operation.record_id,
            .reason = "preserve immediate-ready control ingress while canonicalizing observed Prefetch",
        });
        ++audit.dependency_count;
    }
    else {
        append_edge(plan,
                    core::DagNodeRef::existing(*operation.control_ready_anchor_node_id),
                    core::DagNodeRef::synthetic(join),
                    operation.record_id,
                    "target-observed control readiness supplies the completion-join control branch",
                    audit);
    }
    append_edge(plan,
                core::DagNodeRef::synthetic(join),
                core::DagNodeRef::existing(*operation.wait_exit_anchor_node_id),
                operation.record_id,
                "target-observed Prefetch wait exits after canonical service completion",
                audit);
    output = CanonicalPrefetch{ .operation = &operation, .service_id = service, .join_id = join };
    return true;
}

DirectAudit append_observed_direct_plan(const core::DagGraph & graph, const HiCacheIoOperationLedger & operations,
                                        const model::HiCachePhaseWorkLedger & phase_work, core::DagMutationPlan & plan) {
    DirectAudit audit;
    audit.observed_operation_count = operations.records.size();
    std::vector<CanonicalPrefetch> prefetched;
    for (const auto & operation : operations.records) {
        if (operation.kind != HiCacheIoOperationKind::Prefetch || operation.completed_token_count == 0) continue;
        if (!patch::hicache_io_operation_record_ready(operation)) {
            add_blocker(audit, "prefetch_operation_not_ready");
            continue;
        }
        CanonicalPrefetch record;
        if (append_prefetch(graph, operation, plan, audit, record)) prefetched.push_back(std::move(record));
    }
    std::ranges::sort(prefetched, [](const auto & left, const auto & right) {
        if (left.operation->cache_scope != right.operation->cache_scope) return left.operation->cache_scope < right.operation->cache_scope;
        if (left.operation->source_start_us != right.operation->source_start_us) return left.operation->source_start_us < right.operation->source_start_us;
        return left.operation->record_id < right.operation->record_id;
    });
    std::map<std::string, const CanonicalPrefetch *> prior_by_scope;
    for (const auto & current : prefetched) {
        const auto prior = prior_by_scope.find(current.operation->cache_scope);
        if (prior != prior_by_scope.end()) {
            append_edge(plan,
                        core::DagNodeRef::synthetic(prior->second->service_id),
                        core::DagNodeRef::synthetic(current.service_id),
                        current.operation->cache_scope + ":observed_storage_read_lane",
                        "serialize target-observed Prefetch services on one rank-local storage-read resource",
                        audit);
        }
        prior_by_scope[current.operation->cache_scope] = &current;
    }

    using RequestKey = std::pair<std::string, std::string>;
    std::map<RequestKey, const CanonicalPrefetch *> prefetch_by_request_rank;
    for (const auto & prefetch : prefetched) prefetch_by_request_rank.emplace(RequestKey{ prefetch.operation->pid, prefetch.operation->request_id }, &prefetch);
    std::map<RequestKey, const model::HiCachePrefillWorkItem *> prefill_by_request_rank;
    for (const auto & prefill : phase_work.prefills) prefill_by_request_rank.emplace(RequestKey{ prefill.pid, prefill.request_id }, &prefill);
    for (const auto & operation : operations.records) {
        if (operation.kind != HiCacheIoOperationKind::Load) continue;
        const RequestKey key{ operation.pid, operation.request_id };
        const auto prefetch = prefetch_by_request_rank.find(key);
        const auto prefill = prefill_by_request_rank.find(key);
        if (prefetch == prefetch_by_request_rank.end() || prefill == prefill_by_request_rank.end()) continue;
        if (!patch::hicache_io_operation_record_ready(operation) || operation.device_transfer_node_ids.empty() || !operation.source_readiness_topology_ready) {
            add_blocker(audit, "loadback_readiness_contract_incomplete");
            continue;
        }
        const auto control_tail = operation.admission_explicit_node_ids.empty()
                                      ? operation.admission_explicit_node_ids.end()
                                      : std::ranges::max_element(operation.admission_explicit_node_ids, {}, [&](size_t node_id) {
                                            const auto & event = graph.event_for_node(node_id);
                                            return std::pair{ event.ts + event.dur, node_id };
                                        });
        for (const auto transfer_node_id : operation.device_transfer_node_ids) {
            append_edge(plan,
                        core::DagNodeRef::synthetic(prefetch->second->service_id),
                        core::DagNodeRef::existing(transfer_node_id),
                        operation.record_id,
                        "request Load/H2D cannot precede its target-observed Prefetch service",
                        audit);
            if (control_tail != operation.admission_explicit_node_ids.end()) {
                append_edge(plan,
                            core::DagNodeRef::existing(*control_tail),
                            core::DagNodeRef::existing(transfer_node_id),
                            operation.record_id,
                            "target-observed Load/H2D waits for its explicit active admission-control tail",
                            audit);
            }
            append_edge(plan,
                        core::DagNodeRef::existing(transfer_node_id),
                        core::DagNodeRef::synthetic(
                            hicache_phase_carrier_synthetic_id(prefill->second->request_id, prefill->second->logical_input, "prefill", "common_kernel")),
                        operation.record_id,
                        "request Prefill cannot begin before every target-observed Load/H2D transfer completes",
                        audit);
        }
    }
    std::map<RequestKey, const model::HiCacheDecodeWorkItem *> decode_by_request_rank;
    for (const auto & decode : phase_work.decodes) decode_by_request_rank.emplace(RequestKey{ decode.pid, decode.request_id }, &decode);
    struct RequestBoundary {
        int logical_input = 0;
        uint64_t order_ts = 0;
        std::string request_id;
        const CanonicalPrefetch * prefetch = nullptr;
    };
    std::vector<RequestBoundary> boundaries;
    for (const auto & prefill : phase_work.prefills) {
        const RequestKey key{ prefill.pid, prefill.request_id };
        const auto prefetch = prefetch_by_request_rank.find(key);
        const auto decode = decode_by_request_rank.find(key);
        if (decode == decode_by_request_rank.end() || prefill.submit_cost.source_node_ids.empty() || decode->second->submit_cost.source_node_ids.empty())
            continue;
        const auto first_prefill = std::ranges::min_element(prefill.submit_cost.source_node_ids, {}, [&](size_t node_id) {
            return std::pair{ graph.event_for_node(node_id).ts, node_id };
        });
        boundaries.push_back(RequestBoundary{
            .logical_input = prefill.logical_input,
            .order_ts = graph.event_for_node(*first_prefill).ts,
            .request_id = prefill.request_id,
            .prefetch = prefetch == prefetch_by_request_rank.end() ? nullptr : prefetch->second,
        });
    }
    std::ranges::sort(boundaries, [](const auto & left, const auto & right) {
        if (left.logical_input != right.logical_input) return left.logical_input < right.logical_input;
        if (left.order_ts != right.order_ts) return left.order_ts < right.order_ts;
        return left.request_id < right.request_id;
    });
    std::map<int, const RequestBoundary *> previous_by_input;
    for (const auto & current : boundaries) {
        const auto previous = previous_by_input.find(current.logical_input);
        if (previous != previous_by_input.end() && current.prefetch != nullptr) {
            append_edge(plan,
                        core::DagNodeRef::synthetic(
                            hicache_phase_carrier_synthetic_id(previous->second->request_id, previous->second->logical_input, "decode", "collective")),
                        core::DagNodeRef::synthetic(current.prefetch->service_id),
                        "hicache_observed_request_prefetch_boundary:" + previous->second->request_id + "->" + current.request_id + ":"
                            + std::to_string(current.logical_input),
                        "the next sequential formal request starts Direct Prefetch after prior Decode completion",
                        audit);
        }
        previous_by_input[current.logical_input] = &current;
    }
    audit.canonical_prefetch_count = prefetched.size();
    audit.status = audit.blockers.empty() ? "ready" : "blocked";
    return audit;
}

void own_nodes(core::DagGraph & graph, const std::vector<size_t> & nodes) {
    for (const auto node_id : nodes) graph.set_scope_node_owned(node_id);
}

void mark_noncanonical_direct_scope(core::DagGraph & graph, const HiCacheIoOperationLedger & operations) {
    std::map<size_t, std::vector<std::pair<uint64_t, uint64_t>>> gaps;
    const auto add_gaps = [&](const std::vector<HiCacheCpuGapSlice> & slices) {
        for (const auto & slice : slices) {
            if (slice.owned_end_us > slice.owned_start_us) gaps[slice.owner_node_id].emplace_back(slice.owned_start_us, slice.owned_end_us);
        }
    };
    for (const auto & operation : operations.records) {
        // Canonical Prefetch replaces service/wait carriers, not the explicit
        // terminal CPU children reached after its completion join.
        own_nodes(graph, operation.terminal_control_node_ids);
        if (canonical_prefetch(operation)) continue;
        if (operation.kind == HiCacheIoOperationKind::Load) {
            // Load timing spans are host submissions, not foreground service
            // windows.  The production patch owns calibrated admission control
            // plus the existing device-transfer/readiness closure; Python self
            // time and idle submission gaps remain residual.
            own_nodes(graph, operation.admission_explicit_node_ids);
            own_nodes(graph, operation.device_transfer_node_ids);
            own_nodes(graph, operation.device_completion_node_ids);
            own_nodes(graph, operation.readiness_join_node_ids);
            continue;
        }
        own_nodes(graph, operation.runtime_node_ids);
        own_nodes(graph, operation.admission_explicit_node_ids);
        own_nodes(graph, operation.device_transfer_node_ids);
        own_nodes(graph, operation.device_completion_node_ids);
        own_nodes(graph, operation.readiness_join_node_ids);
        own_nodes(graph, operation.completion_wait_owned_node_ids);
        add_gaps(operation.cpu_gap_slices);
    }
    for (auto & [node_id, intervals] : gaps) {
        std::ranges::sort(intervals);
        uint64_t total = 0;
        uint64_t begin = 0;
        uint64_t end = 0;
        bool active = false;
        for (const auto & interval : intervals) {
            if (!active || interval.first > end) {
                if (active) total = core::checked_add_u64(total, end - begin, "Observed Direct scope gap exceeds uint64 range");
                begin = interval.first;
                end = interval.second;
                active = true;
            }
            else end = std::max(end, interval.second);
        }
        if (active) total = core::checked_add_u64(total, end - begin, "Observed Direct scope gap exceeds uint64 range");
        graph.add_scope_gap_duration(node_id, total);
    }
}

} // namespace

std::string_view HiCacheObservedPhaseCarrierModule::name() const noexcept { return "HiCacheObservedPhaseCarrierModule"; }

void HiCacheObservedPhaseCarrierModule::apply(core::DagGraph & graph) {
    result_.observed = observe_hicache_phases(graph);
    const patch::HiCacheSourceDagIndex source(graph);
    const auto operations = patch::build_hicache_io_operation_ledger(source);
    auto work = observed_phase_work(result_.observed);
    core::DagMutationPlan plan{
        .component = "hicache_observed_canonical_carrier",
        .reason = "score target-observed Direct and phase work with production semantic carriers",
    };
    result_.direct = append_observed_direct_plan(graph, operations, work, plan);
    result_.carrier = append_hicache_phase_carrier_plan(graph, work, plan);
    if (result_.direct.status != "ready" || result_.carrier.status != "ready") {
        result_.status = "blocked";
        applied_ = true;
        return;
    }
    auto mutation = core::apply_dag_mutation_plan(graph, plan);
    result_.journal = mutation.journal;
    result_.topology = std::move(mutation.topology);
    graph.clear_scope_ownership();
    mark_noncanonical_direct_scope(graph, operations);
    for (const auto & record : result_.journal.records) {
        if (!record.node_id) continue;
        if (record.action == core::DagMutationAction::AddSyntheticNode || record.action == core::DagMutationAction::SetNodeDuration)
            graph.set_scope_node_owned(*record.node_id);
    }
    result_.status = result_.topology.ok() ? "applied" : "blocked";
    applied_ = true;
}

} // namespace markov::trace_graph::modules::hicache

#endif
