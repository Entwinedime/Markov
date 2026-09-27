/**
 * @file
 * @brief Historical static HiCache patch used by diagnostic oracle replay.
 */
#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"

#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/phase_carrier.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache {

HiCacheDagPatchModule::HiCacheDagPatchModule(std::shared_ptr<const model::HiCacheModelResult> model_result, bool source_target_same_config)
    : model_result_(std::move(model_result)),
      source_target_same_config_(source_target_same_config) {
    if (!model_result_) throw std::invalid_argument("HiCacheDagPatchModule requires a shared model result");
}

HiCacheDagPatchModule::HiCacheDagPatchModule(std::shared_ptr<const model::HiCacheModelResult> model_result, bool source_target_same_config,
                                             std::string oracle_cost_replay_path,
                                             std::string phase_oracle_cost_replay_path)
    : HiCacheDagPatchModule(std::move(model_result), source_target_same_config) {
    oracle_cost_replay_path_ = std::move(oracle_cost_replay_path);
    phase_oracle_cost_replay_path_ = std::move(phase_oracle_cost_replay_path);
}

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

} // namespace

bool append_hicache_reused_loadback_dependencies(const model::HiCachePhaseWorkLedger & phase_work,
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
        // Reused Record/WAIT edges already express layer readiness.
        // A transfer-to-phase-start edge would serialize all layers.
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
        }
    }
    return true;
}

namespace {

std::string prefill_phase_effect(const model::HiCachePrefillWorkItem & item, std::string_view family) {
    return "hicache_phase:" + item.request_id + ":prefill:" + std::to_string(item.logical_input) + ":" + std::string(family);
}

std::string decode_phase_effect(const model::HiCacheDecodeWorkItem & item, std::string_view family) {
    return "hicache_phase:" + item.request_id + ":decode:" + std::to_string(item.logical_input) + ":" + std::string(family);
}

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
    std::unordered_map<std::string, uint64_t> supplied_attention;
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
            if (row.contains("paged_attention_duration_us")) {
                if (!effect.ends_with(":kernel") || !row.at("paged_attention_duration_us").is_number_unsigned()
                    || row.at("paged_attention_duration_us").get<uint64_t>() > supplied.at(effect))
                    throw std::invalid_argument("invalid Decode attention oracle-cost component");
                supplied_attention.emplace(effect, row.at("paged_attention_duration_us").get<uint64_t>());
            }
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
        const auto kernel_effect = decode_phase_effect(item, "kernel");
        apply(item.kernel_cost, kernel_effect);
        const auto attention = supplied_attention.find(kernel_effect);
        if (attention == supplied_attention.end())
            throw std::invalid_argument("missing Decode attention oracle cost for effect: " + kernel_effect);
        item.predicted_paged_attention_duration_us = attention->second;
        apply(item.collective_cost, decode_phase_effect(item, "collective"));
        if (include_control) apply(item.submit_cost, decode_phase_effect(item, "submit"));
    }
    if (consumed.size() != supplied.size()) throw std::invalid_argument("HiCache phase oracle-cost replay contains unknown effects");
    audit.effect_identity_exact = audit.required_cost_count == audit.supplied_cost_count
                                  && audit.applied_cost_count == audit.required_cost_count;
    audit.status = audit.effect_identity_exact ? "ready" : "invalid";
}

} // namespace

void HiCacheDagPatchModule::apply(core::DagGraph & graph) {
    if (!model_result_->replay_complete) throw std::logic_error("HiCacheDagPatchModule must run after HiCacheModule");

    result_.source_target_same_config = source_target_same_config_;
    auto phase_work = model_result_->phase_work;
    apply_phase_oracle_cost_replay(phase_work, phase_oracle_cost_replay_path_, result_.phase_oracle_cost_replay);
    const auto source_index = patch::HiCacheSourceDagIndex(graph);
    result_.io_operation_ledger = patch::build_hicache_io_operation_ledger(source_index);
    result_.source_attribution = patch::build_hicache_source_attribution(source_index, model_result_->effect_decisions, result_.io_operation_ledger);
    result_.io_resources = patch::build_hicache_io_resource_plan(model_result_->effect_decisions, model_result_->io_cost_model);
    patch::apply_hicache_oracle_cost_replay(result_.io_resources, oracle_cost_replay_path_);
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
            && append_hicache_reused_loadback_dependencies(phase_work, result_.shadow_rewrite, result_.plan)
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
            preparation.added_cost_us = 0;
        }
    }
    if (result_.apply_blockers.empty() && !source_target_same_config_) {
        result_.layer_wait_patch = patch::append_hicache_layer_wait_plan(source_index,
            observe_hicache_layer_waits(source_index), result_.shadow_rewrite.decisions, result_.plan);
        if (result_.layer_wait_patch.status == "blocked") add_apply_blocker(result_, "layer_wait_transition_incomplete");
    }
    // Source component labels are not target costs or mutation claims. Source
    // attribution is complete; communication composition checks actual edits.
    if (!source_target_same_config_) graph.clear_scope_gap_ownership();
    if (result_.apply_blockers.empty()) {
        result_.cpu_collective_patch = patch::append_retained_cpu_collectives(source_index, observe_cpu_collectives(source_index), result_.plan);
        if (result_.cpu_collective_patch.status == "blocked") add_apply_blocker(result_, "cpu_collective_composition_failed");
    }
    if (!result_.apply_blockers.empty()) result_.plan = blocked_plan();
    auto mutation = core::apply_dag_mutation_plan(graph, result_.plan);
    if (result_.apply_blockers.empty()) {
        const bool materialized_topology_valid = mutation.topology.ok();
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
            for (const auto & [reason, count] : result_.applied_validation.blocker_counts)
                detail += "; " + reason + "=" + std::to_string(count);
            size_t sample_count = 0;
            for (const auto & record : result_.applied_validation.records) {
                if (record.ready || sample_count >= 5) continue;
                detail += "; effect=" + record.effect_id + ", rewrite=" + patch::hicache_rewrite_kind_name(record.rewrite_kind) + ", source_duration_exact="
                          + std::to_string(record.source_duration_exact) + ", synthetic_cost_exact=" + std::to_string(record.synthetic_cost_exact)
                          + ", observable_endpoint_exact=" + std::to_string(record.observable_endpoint_exact) + ", ingress_exact="
                          + std::to_string(record.ingress_exact) + ", consumer_dependency_exact=" + std::to_string(record.consumer_dependency_exact);
                if (!record.ingress_exact) {
                    const auto decision = std::ranges::find(result_.shadow_rewrite.decisions, record.effect_id, &patch::HiCacheRewriteDecision::effect_id);
                    if (decision != result_.shadow_rewrite.decisions.end() && decision->completion_control_ingress_edge_id) {
                        const auto id = *decision->completion_control_ingress_edge_id;
                        const auto & edge = graph.edge(id);
                        detail += ", control_ingress=" + std::to_string(id)
                                  + ", original_src=" + std::to_string(edge.src) + ":" + graph.event_for_node(edge.src).name
                                  + ", src_active=" + std::to_string(graph.node(edge.src).active)
                                  + ", original_dst=" + std::to_string(edge.dst) + ":" + graph.event_for_node(edge.dst).name
                                  + ", dst_active=" + std::to_string(graph.node(edge.dst).active);
                    }
                }
                ++sample_count;
            }
            throw std::logic_error(detail);
        }
    }
    result_.journal = std::move(mutation.journal);
    for (const auto & record : result_.journal.records) {
        if (!record.node_id) continue;
        // Ordinary CPU communication is not automatically HiCache-owned cost.
        if (record.effect_id == "cpu_collective") continue;
        if (record.action == core::DagMutationAction::AddSyntheticNode || record.action == core::DagMutationAction::SetNodeDuration)
            graph.set_scope_node_owned(*record.node_id);
    }
    result_.topology = std::move(mutation.topology);
    if (!result_.apply_blockers.empty()) result_.status = "blocked";
    else result_.status = result_.plan.empty() ? "no_mutation_required" : "applied";
    applied_ = true;
}

bool HiCacheDagPatchModule::has_summary() const { return applied_; }

} // namespace markov::trace_graph::modules::hicache
