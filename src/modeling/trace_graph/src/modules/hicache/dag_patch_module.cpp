/**
 * @file
 * @brief Read-only HiCache source attribution and resource planning.
 */
#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"

#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/phase_carrier.hpp"

#ifdef DEBUG
#include "markov/trace_graph/simulation/topological_simulator.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#endif
#include <stdexcept>
#include <algorithm>
#include <charconv>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <utility>

namespace markov::trace_graph::modules::hicache {

HiCacheDagPatchModule::HiCacheDagPatchModule(std::shared_ptr<const model::HiCacheModelResult> model_result, bool source_target_same_config)
    : model_result_(std::move(model_result)),
      source_target_same_config_(source_target_same_config) {
    if (!model_result_) throw std::invalid_argument("HiCacheDagPatchModule requires a shared model result");
}

#ifdef DEBUG
HiCacheDagPatchModule::HiCacheDagPatchModule(std::shared_ptr<const model::HiCacheModelResult> model_result, bool source_target_same_config,
                                             std::string oracle_cost_replay_path,
                                             std::string phase_oracle_cost_replay_path)
    : HiCacheDagPatchModule(std::move(model_result), source_target_same_config) {
    oracle_cost_replay_path_ = std::move(oracle_cost_replay_path);
    phase_oracle_cost_replay_path_ = std::move(phase_oracle_cost_replay_path);
}
#endif

std::string_view HiCacheDagPatchModule::name() const noexcept { return "HiCacheDagPatchModule"; }

namespace {

void add_apply_blocker(HiCacheDagPatchResult & result, std::string blocker) {
    (void)core::checked_increment_u64(result.apply_blockers[std::move(blocker)], "HiCache patch apply-blocker count exceeds uint64 range");
}

void build_apply_gate(HiCacheDagPatchResult & result, const model::HiCacheModelResult & model_result) {
    if (model_result.effect_decisions.status != "ready") add_apply_blocker(result, "target_decision_ledger_not_ready");
    if (result.io_resources.status != "ready") add_apply_blocker(result, "io_resource_plan_not_ready");
    if (!result.source_target_same_config && result.source_attribution.status != "ready") add_apply_blocker(result, "source_attribution_not_ready");
    if (result.shadow_rewrite.status != "ready") add_apply_blocker(result, "shadow_rewrite_not_ready");
    if (!result.shadow_rewrite.topology_valid) add_apply_blocker(result, "shadow_topology_invalid");
    if (result.boundary_validation.status != "ready") add_apply_blocker(result, "boundary_validation_not_ready");
}

core::DagMutationPlan blocked_plan() {
    return core::DagMutationPlan{
        .component = "hicache_direct",
        .reason = "production apply gates are not satisfied",
    };
}

core::DagMutationPlan executable_plan(const patch::HiCacheShadowRewriteTransaction & shadow) {
    auto plan = shadow.plan;
    plan.reason = "complete target-derived HiCache direct-effect transaction";
    return plan;
}

std::optional<int> logical_input_for_scope(std::string_view scope) {
    constexpr std::string_view prefix = "scope:";
    if (!scope.starts_with(prefix)) return std::nullopt;
    uint64_t one_based = 0;
    const auto value = scope.substr(prefix.size());
    const auto [position, error] = std::from_chars(value.data(), value.data() + value.size(), one_based);
    if (error != std::errc{} || position != value.data() + value.size() || one_based == 0
        || one_based - 1 > static_cast<uint64_t>(std::numeric_limits<int>::max()))
        return std::nullopt;
    return static_cast<int>(one_based - 1);
}

bool append_sequential_request_prefetch_boundaries(const core::DagGraph & graph,
                                                   const model::HiCachePhaseWorkLedger & phase_work,
                                                   const patch::HiCacheShadowRewriteTransaction & shadow,
                                                   core::DagMutationPlan & plan) {
    std::set<std::string> synthetic_ids;
    for (const auto & node : plan.synthetic_nodes) synthetic_ids.insert(node.synthetic_id);
    using RequestKey = std::pair<int, std::string>;
    std::map<RequestKey, std::string> prefetch_by_request_rank;
    for (const auto & decision : shadow.decisions) {
        if (decision.effect_type != model::HiCacheEffectType::PrefetchIo || decision.request_id.empty()
            || decision.synthetic_id.empty() || !synthetic_ids.contains(decision.synthetic_id))
            continue;
        const auto logical_input = logical_input_for_scope(decision.cache_scope);
        if (!logical_input || !prefetch_by_request_rank.emplace(RequestKey{ *logical_input, decision.request_id }, decision.synthetic_id).second)
            return false;
    }
    struct Boundary {
        int logical_input = 0;
        uint64_t order_ts = 0;
        std::string request_id;
        std::string prefetch_synthetic_id;
    };
    std::vector<Boundary> boundaries;
    for (const auto & prefill : phase_work.prefills) {
        const auto prefetch = prefetch_by_request_rank.find(RequestKey{ prefill.logical_input, prefill.request_id });
        if (prefill.submit_cost.source_node_ids.empty()) return false;
        const auto first_submit = std::ranges::min_element(prefill.submit_cost.source_node_ids, {}, [&](size_t node_id) {
            return std::pair{ graph.event_for_node(node_id).ts, node_id };
        });
        boundaries.push_back(Boundary{
            .logical_input = prefill.logical_input,
            .order_ts = graph.event_for_node(*first_submit).ts,
            .request_id = prefill.request_id,
            .prefetch_synthetic_id = prefetch == prefetch_by_request_rank.end() ? std::string{} : prefetch->second,
        });
    }
    std::ranges::sort(boundaries, [](const auto & left, const auto & right) {
        if (left.logical_input != right.logical_input) return left.logical_input < right.logical_input;
        if (left.order_ts != right.order_ts) return left.order_ts < right.order_ts;
        return left.request_id < right.request_id;
    });
    std::map<int, const Boundary *> previous_by_input;
    for (const auto & current : boundaries) {
        const auto previous = previous_by_input.find(current.logical_input);
        if (previous != previous_by_input.end() && !current.prefetch_synthetic_id.empty()) {
            plan.add_edges.push_back(core::DagAddEdgeMutation{
                .src = core::DagNodeRef::synthetic(hicache_phase_carrier_synthetic_id(previous->second->request_id,
                                                                                      previous->second->logical_input,
                                                                                      "decode",
                                                                                      "complete")),
                .dst = core::DagNodeRef::synthetic(current.prefetch_synthetic_id),
                .kind = core::DagEdgeKind::Mutation,
                .effect_id = "hicache_request_phase_direct_boundary",
                .reason = "the next sequential formal request starts Direct Prefetch after prior Decode completion",
            });
        }
        previous_by_input[current.logical_input] = &current;
    }
    return true;
}

bool append_reused_loadback_phase_boundaries(const model::HiCachePhaseWorkLedger & phase_work,
                                             const patch::HiCacheShadowRewriteTransaction & shadow,
                                             core::DagMutationPlan & plan) {
    std::set<std::string> synthetic_ids;
    for (const auto & node : plan.synthetic_nodes) synthetic_ids.insert(node.synthetic_id);
    using RequestKey = std::pair<int, std::string>;
    std::map<RequestKey, std::string> prefetch_by_request_rank;
    for (const auto & decision : shadow.decisions) {
        if (decision.effect_type != model::HiCacheEffectType::PrefetchIo || decision.request_id.empty()
            || decision.synthetic_id.empty() || !synthetic_ids.contains(decision.synthetic_id))
            continue;
        const auto logical_input = logical_input_for_scope(decision.cache_scope);
        const auto & ready = decision.target_host_control_required ? decision.target_host_control_synthetic_id
                              : decision.completion_join_required ? decision.completion_join_synthetic_id : decision.synthetic_id;
        if (!logical_input || !prefetch_by_request_rank.emplace(RequestKey{ *logical_input, decision.request_id }, ready).second)
            return false;
    }
    std::map<RequestKey, const model::HiCachePrefillWorkItem *> prefill_by_request_rank;
    for (const auto & prefill : phase_work.prefills) {
        if (!prefill_by_request_rank.emplace(RequestKey{ prefill.logical_input, prefill.request_id }, &prefill).second) return false;
    }
    for (const auto & decision : shadow.decisions) {
        if (decision.effect_type != model::HiCacheEffectType::Loadback || !decision.source_readiness_topology_reused) continue;
        const auto logical_input = logical_input_for_scope(decision.cache_scope);
        if (!logical_input || decision.request_id.empty() || decision.owned_duration_nodes.empty()) return false;
        const RequestKey key{ *logical_input, decision.request_id };
        const auto prefill = prefill_by_request_rank.find(key);
        if (prefill == prefill_by_request_rank.end()) return false;
        const auto prefetch = prefetch_by_request_rank.find(key);
        const auto phase_carrier = hicache_phase_carrier_synthetic_id(prefill->second->request_id,
                                                                      prefill->second->logical_input,
                                                                      "prefill",
                                                                      "start");
        for (const auto transfer_node_id : decision.owned_duration_nodes) {
            if (prefetch != prefetch_by_request_rank.end()) {
                plan.add_edges.push_back(core::DagAddEdgeMutation{
                    .src = core::DagNodeRef::synthetic(prefetch->second),
                    .dst = core::DagNodeRef::existing(transfer_node_id),
                    .kind = core::DagEdgeKind::Mutation,
                    .effect_id = "hicache_request_io_dependency",
                    .reason = "reused target Load/H2D cannot precede ready request Prefetch terminal control",
                });
            }
            plan.add_edges.push_back(core::DagAddEdgeMutation{
                .src = core::DagNodeRef::existing(transfer_node_id),
                .dst = core::DagNodeRef::synthetic(phase_carrier),
                .kind = core::DagEdgeKind::Mutation,
                .effect_id = "hicache_request_phase_direct_boundary",
                .reason = "request Prefill cannot begin before every reused target Load/H2D transfer completes",
            });
        }
    }
    return true;
}

std::string prefill_phase_effect(const model::HiCachePrefillWorkItem & item, std::string_view family) {
    return "hicache_phase:" + item.request_id + ":prefill:" + std::to_string(item.logical_input) + ":" + std::string(family);
}

std::string decode_phase_effect(const model::HiCacheDecodeWorkItem & item, std::string_view family) {
    return "hicache_phase:" + item.request_id + ":decode:" + std::to_string(item.logical_input) + ":" + std::string(family);
}

#ifdef DEBUG
void apply_phase_oracle_cost_replay(model::HiCachePhaseWorkLedger & phase_work,
                                    const std::string & filename,
                                    HiCachePhaseOracleCostReplayAudit & audit) {
    if (filename.empty()) return;
    std::ifstream stream(filename);
    if (!stream) throw std::invalid_argument("cannot open HiCache phase oracle-cost replay input: " + filename);
    nlohmann::json root;
    stream >> root;
    if (!root.is_object() || !root.contains("phase_costs") || !root.at("phase_costs").is_array())
        throw std::invalid_argument("HiCache phase oracle-cost replay requires phase_costs");
    for (std::string_view forbidden : { "target_e2e_us", "actual_e2e_us", "target_prediction_us" }) {
        if (root.contains(std::string(forbidden)))
            throw std::invalid_argument("HiCache phase oracle-cost replay must not consume target E2E fields");
    }
    std::unordered_map<std::string, uint64_t> supplied;
    const auto load_costs = [&](std::string_view field) {
        if (!root.contains(std::string(field))) return false;
        const auto & rows = root.at(std::string(field));
        if (!rows.is_array()) throw std::invalid_argument("HiCache phase oracle-cost collection must be an array");
        for (const auto & row : rows) {
            if (!row.is_object() || !row.contains("effect_id") || !row.at("effect_id").is_string()
                || !row.contains("duration_us") || !row.at("duration_us").is_number_unsigned())
                throw std::invalid_argument("invalid HiCache phase oracle-cost record");
            const auto effect = row.at("effect_id").get<std::string>();
            if (effect.empty() || !supplied.emplace(effect, row.at("duration_us").get<uint64_t>()).second)
                throw std::invalid_argument("duplicate or empty HiCache phase oracle-cost effect");
        }
        return true;
    };
    (void)load_costs("phase_costs");
    const bool include_control = load_costs("phase_control_costs");
    audit.status = "validating";
    audit.supplied_cost_count = supplied.size();
    std::unordered_set<std::string> consumed;
    const auto apply = [&](model::HiCachePhaseNodeCostPlan & cost, const std::string & effect) {
        ++audit.required_cost_count;
        const auto found = supplied.find(effect);
        if (found == supplied.end()) throw std::invalid_argument("missing HiCache phase oracle cost for effect: " + effect);
        cost.predicted_duration_us = found->second;
        consumed.insert(effect);
        audit.oracle_duration_us = core::checked_add_u64(audit.oracle_duration_us,
                                                         found->second,
                                                         "HiCache phase oracle duration exceeds uint64 range");
        audit.applied_duration_us = core::checked_add_u64(audit.applied_duration_us,
                                                          cost.predicted_duration_us,
                                                          "HiCache applied phase oracle duration exceeds uint64 range");
        ++audit.applied_cost_count;
    };
    for (auto & item : phase_work.prefills) {
        apply(item.common_kernel_cost, prefill_phase_effect(item, "common_kernel"));
        apply(item.prefix_attention_cost, prefill_phase_effect(item, "prefix_attention"));
        apply(item.collective_cost, prefill_phase_effect(item, "collective"));
        if (include_control) apply(item.submit_cost, prefill_phase_effect(item, "submit"));
    }
    for (auto & item : phase_work.decodes) {
        apply(item.kernel_cost, decode_phase_effect(item, "kernel"));
        apply(item.collective_cost, decode_phase_effect(item, "collective"));
        if (include_control) apply(item.submit_cost, decode_phase_effect(item, "submit"));
    }
    if (consumed.size() != supplied.size()) throw std::invalid_argument("HiCache phase oracle-cost replay contains unknown effects");
    audit.effect_identity_exact = audit.required_cost_count == audit.supplied_cost_count
                                  && audit.applied_cost_count == audit.required_cost_count;
    audit.status = audit.effect_identity_exact ? "ready" : "invalid";
}
#endif

} // namespace

void HiCacheDagPatchModule::apply(core::DagGraph & graph) {
    if (!model_result_->replay_complete) throw std::logic_error("HiCacheDagPatchModule must run after HiCacheModule");

    result_.source_target_same_config = source_target_same_config_;
    auto phase_work = model_result_->phase_work;
#ifdef DEBUG
    apply_phase_oracle_cost_replay(phase_work, phase_oracle_cost_replay_path_, result_.phase_oracle_cost_replay);
#endif
    {
        auto source_index = patch::HiCacheSourceDagIndex(graph);
        result_.source_index = source_index.stats();
        result_.io_operation_ledger = patch::build_hicache_io_operation_ledger(source_index);
        result_.source_attribution = patch::build_hicache_source_attribution(source_index, model_result_->effect_decisions, result_.io_operation_ledger);
    }
    result_.io_resources = patch::build_hicache_io_resource_plan(model_result_->effect_decisions, model_result_->io_cost_model);
#ifdef DEBUG
    patch::apply_hicache_oracle_cost_replay(result_.io_resources, oracle_cost_replay_path_);
#endif
    result_.shadow_rewrite = patch::build_hicache_shadow_rewrite_transaction(
        graph, model_result_->effect_decisions, result_.source_attribution, result_.io_resources, source_target_same_config_);
    result_.boundary_validation = patch::validate_hicache_shadow_boundaries(graph, result_.shadow_rewrite);
    build_apply_gate(result_, *model_result_);
    result_.plan = result_.apply_blockers.empty() ? executable_plan(result_.shadow_rewrite) : blocked_plan();
    if (!model_result_->phase_cost_model.enabled) result_.phase_patch_status = "disabled";
    else if (phase_work.status != "ready" || phase_work.cost_status != "ready") {
        result_.phase_patch_status = "blocked";
        add_apply_blocker(result_, "phase_work_or_cost_not_ready");
    }
    else {
        const auto before = result_.plan.set_node_durations.size();
        if (result_.apply_blockers.empty()) result_.phase_carrier = append_hicache_phase_carrier_plan(graph, phase_work, result_.plan);
        result_.phase_duration_update_count = result_.plan.set_node_durations.size() - before;
        result_.phase_owner_conflict_count = result_.phase_carrier.owner_conflict_count;
        if (result_.phase_carrier.status == "ready"
            && append_reused_loadback_phase_boundaries(phase_work, result_.shadow_rewrite, result_.plan)
            && append_sequential_request_prefetch_boundaries(graph, phase_work, result_.shadow_rewrite, result_.plan)) {
            result_.plan.component = "hicache";
            result_.plan.reason = "atomic Direct and Prefill/Decode target transaction";
            result_.phase_patch_status = "ready";
        }
        else {
            result_.phase_patch_status = "blocked";
            add_apply_blocker(result_, result_.phase_owner_conflict_count ? "phase_owner_conflict" : "phase_carrier_or_request_boundary_not_ready");
        }
    }
    if (result_.apply_blockers.empty() && model_result_->phase_cost_model.enabled) {
        auto& preparation = result_.runtime_preparation;
        preparation = runtime::plan_allocator_preparations(graph, phase_work.allocator_calls);
        for (const auto& change : preparation.mutation.set_cpu_gaps) {
            if (std::ranges::any_of(result_.plan.set_cpu_gaps, [&](const auto& existing) { return existing.node_id == change.node_id; }))
                ++preparation.blockers["preparation_conflicts_with_hicache_patch"];
        }
        if (preparation.blockers.empty()) result_.plan.set_cpu_gaps.insert(result_.plan.set_cpu_gaps.end(),
            preparation.mutation.set_cpu_gaps.begin(), preparation.mutation.set_cpu_gaps.end());
        else {
            preparation.status = preparation.status == "unavailable" ? "unavailable" : "partial";
            preparation.mutation.set_cpu_gaps.clear();
            preparation.removed_coverage_us = 0;
        }
    }
    if (!result_.apply_blockers.empty()) result_.plan = blocked_plan();
    auto mutation = core::apply_dag_mutation_plan(graph, result_.plan);
    if (result_.apply_blockers.empty()) {
#ifdef DEBUG
        const bool materialized_topology_valid = mutation.topology.ok();
#else
        constexpr bool materialized_topology_valid = true;
#endif
        result_.applied_validation =
            patch::validate_hicache_applied_patch(
                graph, result_.shadow_rewrite, result_.io_resources, result_.plan, mutation, materialized_topology_valid);
        if (result_.applied_validation.status != "ready") {
            std::string detail = "materialized HiCache DAG patch failed post-apply semantic validation: ready="
                                 + std::to_string(result_.applied_validation.ready_count()) + "/" + std::to_string(result_.applied_validation.records.size())
                                 + ", plan_journal_exact=" + std::to_string(result_.applied_validation.plan_journal_exact)
                                 + ", topology_exact=" + std::to_string(result_.applied_validation.topology_exact)
                                 + ", family_dependencies_exact=" + std::to_string(result_.applied_validation.family_dependencies_exact)
                                 + ", lane_dependencies_exact=" + std::to_string(result_.applied_validation.lane_dependencies_exact);
            size_t sample_count = 0;
            for (const auto & record : result_.applied_validation.records) {
                if (record.ready || sample_count >= 5) continue;
                detail += "; effect=" + record.effect_id + ", rewrite=" + patch::hicache_rewrite_kind_name(record.rewrite_kind) + ", source_duration_exact="
                          + std::to_string(record.source_duration_exact) + ", synthetic_cost_exact=" + std::to_string(record.synthetic_cost_exact)
                          + ", observable_endpoint_exact=" + std::to_string(record.observable_endpoint_exact) + ", ingress_exact="
                          + std::to_string(record.ingress_exact) + ", consumer_dependency_exact=" + std::to_string(record.consumer_dependency_exact);
                ++sample_count;
            }
            throw std::logic_error(detail);
        }
    }
    result_.journal = std::move(mutation.journal);
    if (!source_target_same_config_) graph.clear_scope_gap_ownership();
    for (const auto & record : result_.journal.records) {
        if (!record.node_id) continue;
        if (record.action == core::DagMutationAction::AddSyntheticNode || record.action == core::DagMutationAction::SetNodeDuration)
            graph.set_scope_node_owned(*record.node_id);
    }
#ifdef DEBUG
    result_.topology = std::move(mutation.topology);
#endif
    if (!result_.apply_blockers.empty()) result_.status = "blocked";
    else result_.status = result_.plan.empty() ? "no_mutation_required" : "applied";
#ifdef DEBUG
    applied_ = true;
#endif
}

#ifdef DEBUG
bool HiCacheDagPatchModule::has_summary() const { return applied_; }

void HiCacheDagPatchModule::run_causal_timing_audit(core::DagGraph & graph) {
    auto & audit = result_.causal_timing_audit;
    if (audit.status != "not_run") throw std::logic_error("HiCache causal timing audit may only run once");
    if (result_.io_resources.oracle_cost_replay.status != "ready") {
        audit.status = "disabled_without_oracle_cost";
        return;
    }

    audit.full_with_target_cost_us = graph.e2e_time();
    audit.control_with_target_cost_us = graph.control_e2e_time();
    if (result_.source_target_same_config) {
        audit.status = "self_baseline_preserves_observed_source";
        audit.full_without_target_cost_us = audit.full_with_target_cost_us;
        audit.control_without_target_cost_us = audit.control_with_target_cost_us;
        audit.restored_exact = true;
        return;
    }

    std::unordered_map<std::string, size_t> synthetic_node_ids;
    synthetic_node_ids.reserve(graph.active_synthetic_node_count());
    for (const auto & node : graph.nodes()) {
        if (!node.active || node.kind != core::DagNodeKind::Synthetic) continue;
        const auto synthetic_id = graph.event_for_node(node.id).arg("synthetic_id");
        if (!synthetic_id.empty()) synthetic_node_ids.emplace(synthetic_id, node.id);
    }

    std::unordered_map<std::string, std::vector<std::pair<size_t, uint64_t>>> target_cost_nodes_by_effect;
    std::unordered_map<size_t, std::string> cost_node_owner;
    const auto append_cost_node = [&](const std::string & effect_id, size_t node_id) {
        if (node_id >= graph.node_count() || !graph.node(node_id).active || graph.node(node_id).duration == 0) return;
        const auto [owner, inserted] = cost_node_owner.emplace(node_id, effect_id);
        if (!inserted) {
            if (owner->second != effect_id) throw std::logic_error("HiCache causal audit found one target-cost node owned by multiple effects");
            return;
        }
        target_cost_nodes_by_effect[effect_id].emplace_back(node_id, graph.node(node_id).duration);
    };
    for (const auto & record : result_.journal.records) {
        if (!record.node_id) continue;
        if (record.action != core::DagMutationAction::AddSyntheticNode && record.action != core::DagMutationAction::SetNodeDuration) continue;
        append_cost_node(record.effect_id, *record.node_id);
    }
    for (const auto & decision : result_.shadow_rewrite.decisions) {
        if (!decision.shadow_plan_ready || decision.rewrite_kind == patch::HiCacheRewriteKind::NoOp
            || decision.rewrite_kind == patch::HiCacheRewriteKind::Reject)
            continue;
        if (decision.source_readiness_topology_reused) {
            for (const auto node_id : decision.owned_duration_nodes) append_cost_node(decision.effect_id, node_id);
        }
    }

    struct EffectTimingProbe {
        size_t audit_index = 0;
        std::vector<std::pair<size_t, uint64_t>> cost_completion_with;
        std::vector<std::pair<size_t, uint64_t>> join_start_with;
        std::vector<std::pair<size_t, uint64_t>> consumer_start_with;
    };
    std::vector<EffectTimingProbe> probes;
    const auto append_node_timing = [&](std::vector<std::pair<size_t, uint64_t>> & output, size_t node_id, bool completion) {
        if (node_id >= graph.node_count() || !graph.node(node_id).active) return;
        const auto & node = graph.node(node_id);
        output.emplace_back(node_id, completion ? node.completion_time : node.simulation_start);
    };
    const auto append_synthetic_timing = [&](std::vector<std::pair<size_t, uint64_t>> & output, const std::string & synthetic_id) {
        if (const auto found = synthetic_node_ids.find(synthetic_id); found != synthetic_node_ids.end()) append_node_timing(output, found->second, false);
    };
    for (const auto & decision : result_.shadow_rewrite.decisions) {
        const auto costs = target_cost_nodes_by_effect.find(decision.effect_id);
        if (costs == target_cost_nodes_by_effect.end()) continue;
        HiCacheEffectCausalTimingAudit record{
            .effect_id = decision.effect_id,
            .effect_type = model::hicache_effect_type_name(decision.effect_type),
            .rewrite_kind = patch::hicache_rewrite_kind_name(decision.rewrite_kind),
            .causal_path_kind = decision.target_host_control_terminal             ? "terminal_control_to_consumer"
                                : decision.completion_join_required               ? "completion_join_to_wait_exit"
                                : decision.source_readiness_topology_reused       ? "reused_readiness_to_consumer"
                                : !decision.family_consumer_synthetic_id.empty()  ? "family_effect_consumer"
                                : !decision.consumer_anchors.empty()              ? "direct_consumer"
                                                                                  : "resource_only_background",
            .source_completion_wait_duration_us = decision.completion_wait_duration_us,
            .source_completion_wait_gap_duration_us = decision.completion_wait_gap_duration_us,
            .source_residual_unknown_duration_us = decision.residual_unknown_duration_us,
            .foreground_path_expected = decision.completion_join_required || decision.source_readiness_topology_reused || !decision.consumer_anchors.empty()
                                        || !decision.family_consumer_synthetic_id.empty(),
            .completion_join_required = decision.completion_join_required,
            .source_readiness_topology_reused = decision.source_readiness_topology_reused,
            .source_completion_wait_blocking = decision.source_completion_wait_blocking,
        };
        EffectTimingProbe probe{ .audit_index = audit.effects.size() };
        for (const auto & [node_id, duration] : costs->second) {
            record.target_cost_duration_us =
                core::checked_add_u64(record.target_cost_duration_us, duration, "HiCache per-effect causal-audit target duration exceeds uint64 range");
            append_node_timing(probe.cost_completion_with, node_id, true);
        }
        record.target_cost_node_count = costs->second.size();
        if (decision.completion_join_required) append_synthetic_timing(probe.join_start_with, decision.completion_join_synthetic_id);
        if (decision.target_host_control_terminal) {
            for (const auto node_id : decision.consumer_anchors) append_node_timing(probe.consumer_start_with, node_id, false);
        }
        else if (decision.wait_exit_anchor_node_id) append_node_timing(probe.consumer_start_with, *decision.wait_exit_anchor_node_id, false);
        else {
            for (const auto node_id : decision.consumer_anchors) append_node_timing(probe.consumer_start_with, node_id, false);
            append_synthetic_timing(probe.consumer_start_with, decision.family_consumer_synthetic_id);
        }
        record.completion_join_node_count = probe.join_start_with.size();
        record.consumer_node_count = probe.consumer_start_with.size();
        audit.target_cost_node_count =
            core::checked_add_u64(audit.target_cost_node_count, record.target_cost_node_count, "HiCache causal-audit target node count exceeds uint64 range");
        audit.target_cost_duration_us =
            core::checked_add_u64(audit.target_cost_duration_us, record.target_cost_duration_us, "HiCache causal-audit target duration exceeds uint64 range");
        audit.effects.push_back(std::move(record));
        probes.push_back(std::move(probe));
    }

    std::vector<std::pair<size_t, uint64_t>> target_cost_nodes;
    target_cost_nodes.reserve(cost_node_owner.size());
    for (const auto & [effect_id, costs] : target_cost_nodes_by_effect) {
        (void)effect_id;
        target_cost_nodes.insert(target_cost_nodes.end(), costs.begin(), costs.end());
    }
    if (target_cost_nodes.empty()) {
        audit.status = "no_materialized_target_cost_nodes";
        audit.full_without_target_cost_us = audit.full_with_target_cost_us;
        audit.control_without_target_cost_us = audit.control_with_target_cost_us;
        audit.restored_exact = true;
        return;
    }

    const auto restore_costs = [&] {
        for (const auto & [node_id, duration] : target_cost_nodes) graph.set_node_duration(node_id, duration);
    };
    for (const auto & [node_id, duration] : target_cost_nodes) {
        (void)duration;
        graph.set_node_duration(node_id, 0);
    }
    try {
        audit.full_without_target_cost_us = simulation::run_topological_simulation(graph).e2e_us;
        const auto timing_response = [&](const std::vector<std::pair<size_t, uint64_t>> & with_times, bool completion) {
            uint64_t response = 0;
            for (const auto & [node_id, with_time] : with_times) {
                const auto & node = graph.node(node_id);
                const auto without_time = completion ? node.completion_time : node.simulation_start;
                if (without_time > with_time) throw std::logic_error("Removing HiCache target cost delayed a local DAG endpoint");
                response = std::max(response, with_time - without_time);
            }
            return response;
        };
        for (const auto & probe : probes) {
            auto & effect = audit.effects[probe.audit_index];
            effect.cost_node_completion_response_us = timing_response(probe.cost_completion_with, true);
            effect.completion_join_start_response_us = timing_response(probe.join_start_with, false);
            effect.consumer_start_response_us = timing_response(probe.consumer_start_with, false);
            const bool locally_sensitive = effect.completion_join_start_response_us > 0 || effect.consumer_start_response_us > 0;
            if (locally_sensitive) {
                effect.status = "target_cost_controls_local_endpoint";
                (void)core::checked_increment_u64(audit.local_cost_sensitive_effect_count, "HiCache local cost-sensitive effect count exceeds uint64 range");
            }
            else {
                effect.status = effect.foreground_path_expected ? "target_cost_hidden_at_local_endpoint" : "background_without_foreground_endpoint";
                (void)core::checked_increment_u64(audit.local_cost_hidden_effect_count, "HiCache locally hidden effect count exceeds uint64 range");
            }
        }
        audit.control_without_target_cost_us = simulation::run_control_topological_simulation(graph).e2e_us;
    }
    catch (...) {
        restore_costs();
        (void)simulation::run_topological_simulation(graph);
        (void)simulation::run_control_topological_simulation(graph);
        audit.status = "counterfactual_simulation_failed";
        throw;
    }

    restore_costs();
    const auto restored_full = simulation::run_topological_simulation(graph).e2e_us;
    const auto restored_control = simulation::run_control_topological_simulation(graph).e2e_us;
    audit.restored_exact = restored_full == audit.full_with_target_cost_us && restored_control == audit.control_with_target_cost_us;
    if (!audit.restored_exact) throw std::logic_error("HiCache causal timing audit failed to restore the target-cost simulation exactly");
    if (audit.full_without_target_cost_us > audit.full_with_target_cost_us || audit.control_without_target_cost_us > audit.control_with_target_cost_us)
        throw std::logic_error("HiCache target costs cannot reduce a monotone DAG critical path");
    audit.full_target_cost_response_us = audit.full_with_target_cost_us - audit.full_without_target_cost_us;
    audit.control_target_cost_response_us = audit.control_with_target_cost_us - audit.control_without_target_cost_us;
    audit.status = "ready";
}
#endif

} // namespace markov::trace_graph::modules::hicache
