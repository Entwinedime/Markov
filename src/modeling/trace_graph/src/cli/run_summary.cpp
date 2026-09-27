/**
 * @file
 * @brief Serializes compact graph, simulation, and module run results.
 */
#include "run_summary.hpp"
#include "hicache_observations.hpp"

#include "file_output.hpp"

#include "markov/trace_graph/core/numeric.hpp"
#ifdef DEBUG
#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"
#include "markov/trace_graph/modules/hicache/phase_carrier.hpp"
#endif
#include "markov/trace_graph/modules/hicache/hicache_module.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include "markov/trace_graph/modules/node_scale/node_scale_module.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <ranges>
#include <tuple>
#include <unordered_map>

namespace markov::trace_graph::cli {

namespace {

using Json = nlohmann::json;

#ifdef DEBUG
std::string edge_kind_name(core::DagEdgeKind kind) {
    switch (kind) {
    case core::DagEdgeKind::Sequential:
        return "sequential";
    case core::DagEdgeKind::Stream:
        return "stream";
    case core::DagEdgeKind::Correlation:
        return "correlation";
    case core::DagEdgeKind::Sync:
        return "sync";
    case core::DagEdgeKind::HCCL:
        return "hccl";
    case core::DagEdgeKind::HiCache:
        return "hicache";
    case core::DagEdgeKind::Mutation:
        return "mutation";
    }
    return "unknown";
}

std::string event_family(std::string_view name) {
    const auto suffix = name.find("__");
    return std::string(name.substr(0, suffix));
}

Json gap_excluded_critical_path_result(const core::DagGraph & graph, const modules::hicache::HiCachePhaseObservationAudit & phase) {
    struct Contribution {
        uint64_t node_count = 0;
        uint64_t duration_us = 0;
    };
    struct SemanticSegment {
        std::string component;
        int logical_input = 0;
        std::string request_id;
        std::string phase;
        std::string family;
        std::string effect_id;
        uint64_t node_duration_us = 0;
        uint64_t incoming_gap_us = 0;
        uint64_t node_count = 0;
    };
    using Key = std::tuple<int, std::string, std::string, std::string>;
    using PhaseKey = std::tuple<int, std::string, std::string, std::string>;
    std::map<Key, Contribution> contributions;
    std::map<PhaseKey, Contribution> phase_contributions;
    std::unordered_map<size_t, PhaseKey> phase_owner_by_node;
    const auto index_phase_nodes = [&](const auto & observation, const std::vector<size_t> & node_ids, std::string_view phase_name, std::string_view family) {
        if (observation.request_ids.size() != 1) return;
        const PhaseKey key{ observation.logical_input, observation.request_ids.front(), std::string(phase_name), std::string(family) };
        for (const auto node_id : node_ids) phase_owner_by_node.emplace(node_id, key);
    };
    for (const auto & observation : phase.observations) {
        index_phase_nodes(observation, observation.prefill_common_kernel_node_ids, "prefill", "common_kernel");
        index_phase_nodes(observation, observation.prefill_prefix_attention_node_ids, "prefill", "prefix_attention");
        index_phase_nodes(observation, observation.prefill_collective_node_ids, "prefill", "collective");
        index_phase_nodes(observation, observation.prefill_submit_cpu_node_ids, "prefill", "submit");
        index_phase_nodes(observation, observation.decode_kernel_node_ids, "decode", "kernel");
        index_phase_nodes(observation, observation.decode_collective_node_ids, "decode", "collective");
        index_phase_nodes(observation, observation.decode_submit_cpu_node_ids, "decode", "submit");
    }
    std::map<std::string, uint64_t> delay_by_edge_kind;
    std::vector<SemanticSegment> semantic_segments;
    const auto append_semantic_segment = [&](SemanticSegment segment) {
        if (segment.node_duration_us == 0 && segment.incoming_gap_us == 0) return;
        if (!semantic_segments.empty()) {
            auto & previous = semantic_segments.back();
            const bool same_phase =
                previous.component == "phase" && segment.component == "phase" && previous.request_id == segment.request_id && previous.phase == segment.phase;
            const bool same_observed_direct = previous.component == "direct_observed" && segment.component == "direct_observed";
            const bool same_synthetic_direct = previous.component == "direct_synthetic" && segment.component == "direct_synthetic"
                                               && previous.logical_input == segment.logical_input && previous.request_id == segment.request_id
                                               && previous.family == segment.family && previous.effect_id == segment.effect_id;
            if (same_phase || same_observed_direct || same_synthetic_direct) {
                if (previous.logical_input != segment.logical_input) previous.logical_input = -1;
                if (previous.family != segment.family) previous.family = "mixed";
                previous.node_duration_us =
                    core::checked_add_u64(previous.node_duration_us, segment.node_duration_us, "critical-path semantic node duration exceeds uint64 range");
                previous.incoming_gap_us =
                    core::checked_add_u64(previous.incoming_gap_us, segment.incoming_gap_us, "critical-path semantic gap duration exceeds uint64 range");
                previous.node_count = core::checked_add_u64(previous.node_count, segment.node_count, "critical-path semantic node count exceeds uint64 range");
                return;
            }
        }
        semantic_segments.push_back(std::move(segment));
    };
    uint64_t owner_duration_us = 0;
    uint64_t incoming_delay_us = 0;
    size_t contributing_node_count = 0;
    size_t logical_input_transition_count = 0;
    std::optional<int> prior_logical_input;
    for (const auto & step : graph.gap_excluded_critical_path()) {
        if (step.incoming_delay_us > 0 && step.predecessor_node_id < graph.node_count()) {
            const auto & predecessor = graph.node(step.predecessor_node_id);
            const auto & predecessor_event = graph.event_for_node(step.predecessor_node_id);
            append_semantic_segment(SemanticSegment{
                .component = "direct_observed",
                .logical_input = predecessor.gpu_id,
                .request_id = predecessor_event.arg("request_id"),
                .family = event_family(predecessor_event.name),
                .incoming_gap_us = step.incoming_delay_us,
            });
        }
        incoming_delay_us = core::checked_add_u64(incoming_delay_us, step.incoming_delay_us, "critical-path delay exceeds uint64 range");
        if (step.incoming_delay_us > 0)
            delay_by_edge_kind[edge_kind_name(step.incoming_edge_kind)] = core::checked_add_u64(delay_by_edge_kind[edge_kind_name(step.incoming_edge_kind)],
                                                                                                step.incoming_delay_us,
                                                                                                "critical-path edge-kind delay exceeds uint64 range");
        if (step.effective_duration_us == 0) continue;
        const auto & node = graph.node(step.node_id);
        const auto & event = graph.event_for_node(step.node_id);
        const auto execution = node.kind == core::DagNodeKind::Synthetic ? "synthetic" : node.is_cpu ? "cpu" : "device";
        const auto family = event_family(event.name);
        const Key key{ node.gpu_id, execution, family, event.cat };
        auto & contribution = contributions[key];
        ++contribution.node_count;
        contribution.duration_us =
            core::checked_add_u64(contribution.duration_us, step.effective_duration_us, "critical-path family duration exceeds uint64 range");
        owner_duration_us = core::checked_add_u64(owner_duration_us, step.effective_duration_us, "critical-path owner duration exceeds uint64 range");

        std::optional<PhaseKey> phase_key;
        if (const auto owner = phase_owner_by_node.find(step.node_id); owner != phase_owner_by_node.end()) phase_key = owner->second;
        else if (node.kind == core::DagNodeKind::Synthetic && event.cat == "hicache_phase")
            phase_key = PhaseKey{ node.gpu_id, event.arg("request_id"), event.arg("phase"), event.arg("family") };

        if (phase_key) {
            auto & phase_row = phase_contributions[*phase_key];
            ++phase_row.node_count;
            phase_row.duration_us =
                core::checked_add_u64(phase_row.duration_us, step.effective_duration_us, "critical-path phase-owner duration exceeds uint64 range");
            append_semantic_segment(SemanticSegment{
                .component = "phase",
                .logical_input = node.gpu_id,
                .request_id = std::get<1>(*phase_key),
                .phase = std::get<2>(*phase_key),
                .family = std::get<3>(*phase_key),
                .node_duration_us = step.effective_duration_us,
                .node_count = 1,
            });
        }
        else if (node.kind == core::DagNodeKind::Synthetic && event.cat == "hicache_patch") {
            append_semantic_segment(SemanticSegment{
                .component = "direct_synthetic",
                .logical_input = node.gpu_id,
                .request_id = event.arg("request_id"),
                .family = family,
                .effect_id = event.arg("effect_id"),
                .node_duration_us = step.effective_duration_us,
                .node_count = 1,
            });
        }
        else if (family.starts_with("hicache")) {
            append_semantic_segment(SemanticSegment{
                .component = "direct_observed",
                .logical_input = node.gpu_id,
                .request_id = event.arg("request_id"),
                .family = family,
                .node_duration_us = step.effective_duration_us,
                .node_count = 1,
            });
        }
        ++contributing_node_count;
        if (prior_logical_input && *prior_logical_input != node.gpu_id) ++logical_input_transition_count;
        prior_logical_input = node.gpu_id;
    }

    std::vector<std::pair<Key, Contribution>> ordered(contributions.begin(), contributions.end());
    std::ranges::sort(ordered, [](const auto & left, const auto & right) {
        if (left.second.duration_us != right.second.duration_us) return left.second.duration_us > right.second.duration_us;
        return left.first < right.first;
    });

    constexpr size_t kRetainedFamilyCount = 64;
    Json families = Json::array();
    for (const auto & [key, row] : ordered | std::views::take(kRetainedFamilyCount)) {
        families.push_back(Json{
            { "logical_input", std::get<0>(key) },
            {     "execution", std::get<1>(key) },
            {        "family", std::get<2>(key) },
            {      "category", std::get<3>(key) },
            {    "node_count",   row.node_count },
            {   "duration_us",  row.duration_us },
        });
    }

    Json phase_owners = Json::array();
    for (const auto & [key, row] : phase_contributions) {
        phase_owners.push_back(Json{
            { "logical_input", std::get<0>(key) },
            {    "request_id", std::get<1>(key) },
            {         "phase", std::get<2>(key) },
            {        "family", std::get<3>(key) },
            {    "node_count",   row.node_count },
            {   "duration_us",  row.duration_us },
        });
    }

    constexpr size_t kRetainedSemanticSegmentCount = 512;
    Json semantic_sequence = Json::array();
    for (const auto & segment : semantic_segments | std::views::take(kRetainedSemanticSegmentCount)) {
        Json row{
            {        "component",        segment.component },
            {    "logical_input",    segment.logical_input },
            {       "request_id",       segment.request_id },
            {            "phase",            segment.phase },
            {           "family",           segment.family },
            {       "node_count",       segment.node_count },
            { "node_duration_us", segment.node_duration_us },
            {  "incoming_gap_us",  segment.incoming_gap_us },
        };
        if (!segment.effect_id.empty()) row["effect_id"] = segment.effect_id;
        semantic_sequence.push_back(std::move(row));
    }
    const auto contribution_us = core::checked_add_u64(owner_duration_us, incoming_delay_us, "critical-path contribution exceeds uint64 range");
    return Json{
        {                 "path_node_count",           graph.gap_excluded_critical_path().size() },
        {         "contributing_node_count",                             contributing_node_count },
        {               "owner_duration_us",                                   owner_duration_us },
        {               "incoming_delay_us",                                   incoming_delay_us },
        {                 "contribution_us",                                     contribution_us },
        {        "matches_gap_excluded_e2e",    contribution_us == graph.gap_excluded_e2e_time() },
        {  "logical_input_transition_count",                      logical_input_transition_count },
        {                    "family_count",                                      ordered.size() },
        {           "retained_family_count",                                     families.size() },
        {              "delay_by_edge_kind",                                  delay_by_edge_kind },
        {                        "families",                                 std::move(families) },
        {                    "phase_owners",                             std::move(phase_owners) },
        {          "semantic_segment_count",                            semantic_segments.size() },
        { "retained_semantic_segment_count",                            semantic_sequence.size() },
        {     "semantic_sequence_truncated", semantic_segments.size() > semantic_sequence.size() },
        {               "semantic_sequence",                        std::move(semantic_sequence) },
    };
}
#endif

Json effect_decision_result(const modules::hicache::model::HiCacheEffectDecisionLedger & ledger) {
    Json result;
    result["status"] = ledger.status;
    result["decision_count"] = ledger.decisions.size();
    result["patchable_count"] = ledger.patchable_count();
    result["not_patchable_count"] = ledger.not_patchable_count();
    result["deferred_count"] = ledger.deferred_count();
    result["unresolved_count"] = ledger.unresolved_count();
    result["schedule_sensitive_count"] = ledger.schedule_sensitive_count();
    result["decision_coverage"] = ledger.decision_coverage;
    result["counts_by_effect_type"] = ledger.counts_by_effect_type();
    result["counts_by_target_effect_state"] = ledger.counts_by_target_effect_state();
    result["counts_by_schedule_sensitivity"] = ledger.counts_by_schedule_sensitivity();
    result["counts_by_source_carrier_state"] = ledger.counts_by_source_carrier_state();
    result["missing_facts"] = ledger.missing_facts;
    result["not_patchable_reasons"] = ledger.not_patchable_reasons;
    result["byte_projection_available"] = ledger.byte_projection_available;
    result["kv_bytes_per_page"] = ledger.kv_bytes_per_page;
    result["l2_capacity_pages"] = ledger.l2_capacity_pages;
    result["l2_capacity_bytes"] = ledger.l2_capacity_bytes;
    result["byte_projection_source"] = ledger.byte_projection_source;
    return result;
}

Json phase_work_result(const modules::hicache::model::HiCachePhaseWorkLedger & plan) {
    using Cost = modules::hicache::model::HiCachePhaseNodeCostPlan;
    const auto cost = [](const Cost & item, const Cost & other = {}) {
        return Json{
            { "source_duration_us", core::checked_add_u64(item.source_duration_us, other.source_duration_us, "Phase source total overflow") },
            { "predicted_duration_us", core::checked_add_u64(item.predicted_duration_us, other.predicted_duration_us, "Phase predicted total overflow") },
            { "source_node_count", item.source_node_ids.size() + other.source_node_ids.size() },
        };
    };
    Json prefills = Json::array();
    for (const auto & item : plan.prefills) {
        prefills.push_back(Json{
            { "logical_input", item.logical_input },
            { "pid", item.pid },
            { "request_id", item.request_id },
            { "batch_position", item.batch_position },
            { "batch_size", item.batch_size },
            { "target_page_size", item.target_page_size },
            { "prompt_token_count", item.prompt_token_count },
            { "reusable_prefix_token_count", item.reusable_prefix_token_count },
            { "prefill_token_count", item.prefill_token_count },
            { "source_prefill_token_count", item.source_prefill_token_count },
            { "attention_token_pairs", item.attention_token_pairs },
            { "feature_covered", item.feature_covered },
            { "common_kernel_cost", cost(item.common_kernel_cost) },
            { "prefix_attention_cost", cost(item.prefix_attention_cost) },
            { "kernel_cost", cost(item.common_kernel_cost, item.prefix_attention_cost) },
            { "collective_cost", cost(item.collective_cost) },
            { "submit_cost", cost(item.submit_cost) },
        });
    }
    Json decodes = Json::array();
    for (const auto & item : plan.decodes) {
        decodes.push_back(Json{
            {                         "logical_input",                         item.logical_input },
            {                                   "pid",                                   item.pid },
            {                            "request_id",                            item.request_id },
            {                    "prompt_token_count",                    item.prompt_token_count },
            {                      "target_page_size",                      item.target_page_size },
            {                  "effective_page_count",                  item.effective_page_count },
            {                       "iteration_count",                       item.iteration_count },
            {    "source_paged_attention_duration_us",    item.source_paged_attention_duration_us },
            { "predicted_paged_attention_duration_us", item.predicted_paged_attention_duration_us },
            {                       "feature_covered",                       item.feature_covered },
            {                           "kernel_cost",                     cost(item.kernel_cost) },
            {                       "collective_cost",                 cost(item.collective_cost) },
            {                           "submit_cost",                     cost(item.submit_cost) },
        });
    }
    Json allocator_calls = Json::array();
    for (const auto & call : plan.allocator_calls) {
        allocator_calls.push_back({
            {     "source_fact_id",                                                    call.source_fact_id },
            { "source_event_index",                                                call.source_event_index },
            {                "pid",                                                               call.pid },
            {        "request_ids",                                                       call.request_ids },
            {             "formal",                                                            call.formal },
            {          "page_size",                                                         call.page_size },
            {         "batch_size",                                                        call.batch_size },
            {      "extend_tokens",                                                     call.extend_tokens },
            {    "allocated_pages",                                                   call.allocated_pages },
            {  "free_index_offset", call.free_index_offset ? Json(*call.free_index_offset) : Json(nullptr) },
        });
    }
    return Json{
        { "status", plan.status },
        { "prefill_status", plan.prefill_status },
        { "decode_status", plan.decode_status },
        { "cost_status", plan.cost_status },
        { "prefill_feature_covered_count", std::ranges::count_if(plan.prefills, [](const auto & item) { return item.feature_covered; }) },
        { "decode_feature_covered_count", std::ranges::count_if(plan.decodes, [](const auto & item) { return item.feature_covered; }) },
        { "prefill_count", plan.prefills.size() },
        { "decode_count", plan.decodes.size() },
        { "blockers", plan.blockers },
        { "prefills", std::move(prefills) },
        { "decodes", std::move(decodes) },
        { "allocator_calls", std::move(allocator_calls) },
    };
}


#ifdef DEBUG
Json io_resource_result(const modules::hicache::patch::HiCacheIoResourcePlan & resources) {
    Json result{
        {                 "status",                                                                    resources.status },
        { "byte_projection_source", resources.byte_projection_available ? "target_config.kv_bytes_per_page" : "missing" },
        {      "kv_bytes_per_page",                                                         resources.kv_bytes_per_page },
        {         "decision_count",                                                              resources.costs.size() },
        {       "cost_ready_count",                                                             resources.ready_count() },
        {  "lane_dependency_count",                                                  resources.lane_dependencies.size() },
        {  "counts_by_cost_status",                                                          resources.counts_by_status },
        {         "blocker_counts",                                                            resources.blocker_counts },
    };
    const auto & oracle = resources.oracle_cost_replay;
    if (oracle.status != "disabled") {
        result["oracle_cost_replay"] = Json{
            {                "status",                oracle.status },
            {   "required_cost_count",   oracle.required_cost_count },
            {   "supplied_cost_count",   oracle.supplied_cost_count },
            {    "applied_cost_count",    oracle.applied_cost_count },
            {     "oracle_service_us",     oracle.oracle_service_us },
            {    "applied_service_us",    oracle.applied_service_us },
            {     "oracle_control_us",     oracle.oracle_control_us },
            {    "applied_control_us",    oracle.applied_control_us },
            { "effect_identity_exact", oracle.effect_identity_exact },
            { "operation_shape_exact", oracle.operation_shape_exact },
            {   "target_e2e_consumed",   oracle.target_e2e_consumed },
        };
    }
    return result;
}

Json phase_carrier_audit(const modules::hicache::HiCachePhaseCarrierAudit & carrier) {
    return Json{
        {                   "status",                   carrier.status },
        {       "request_rank_count",       carrier.request_rank_count },
        {            "request_count",            carrier.request_count },
        {  "synthetic_carrier_count",  carrier.synthetic_carrier_count },
        { "source_device_node_count", carrier.source_device_node_count },
        { "source_submit_node_count", carrier.source_submit_node_count },
        {         "dependency_count",         carrier.dependency_count },
        {     "owner_conflict_count",     carrier.owner_conflict_count },
        {                 "blockers",                 carrier.blockers },
    };
}

Json dag_patch_result(const modules::hicache::HiCacheDagPatchModule & module) {
    const auto & result = module.result();
    const auto & preparation = result.runtime_preparation;
    std::map<std::string, size_t> preparation_counts;
    for (const auto & call : preparation.calls) ++preparation_counts[call.status];
    std::map<std::string, size_t> required_paths;
    for (const auto & call : preparation.calls)
        if (call.status == "required") ++required_paths[call.path];
    Json preparation_costs = Json::object();
    for (const auto & [pid, paths] : preparation.cost_samples)
        for (const auto & [path, samples] : paths)
            preparation_costs[pid][path] = {
                {      "count",      samples.count },
                { "minimum_us", samples.minimum_us },
                {  "median_us",  samples.median_us },
                { "maximum_us", samples.maximum_us }
            };
    const auto duration_update_count = static_cast<size_t>(
        std::ranges::count_if(result.journal.records, [](const auto & record) { return record.action == core::DagMutationAction::SetNodeDuration; }));
    const auto e2e_eligibility_update_count = static_cast<size_t>(
        std::ranges::count_if(result.journal.records, [](const auto & record) { return record.action == core::DagMutationAction::SetNodeE2eEligibility; }));
    const bool topology_valid = result.shadow_rewrite.topology_valid && result.applied_validation.topology_exact;
    const bool validation_ready = result.source_attribution.status == "ready" && result.shadow_rewrite.status == "ready"
                                  && result.boundary_validation.status == "ready" && result.applied_validation.status == "ready" && topology_valid;
    Json summary{
        {                       "status",result.status                                         },
        {           "phase_patch_status",                          result.phase_patch_status },
        {         "cpu_collective_patch",
         { { "status", result.cpu_collective_patch.status },
         { "observed_rounds", result.cpu_collective_patch.observed_rounds },
         { "planned_rounds", result.cpu_collective_patch.planned_rounds },
         { "source_retained_rounds", result.cpu_collective_patch.source_retained_rounds },
         { "blockers", result.cpu_collective_patch.blockers } }                             },
        {             "layer_wait_patch",
         { { "status", result.layer_wait_patch.status },
         { "retained_calls", result.layer_wait_patch.retained_calls },
         { "removed_calls", result.layer_wait_patch.removed_calls },
         { "required_insertions", result.layer_wait_patch.required_insertions },
         { "blockers", result.layer_wait_patch.blockers },
         { "inserted_calls", result.layer_wait_patch.inserted_calls },
         { "rebound_calls", result.layer_wait_patch.rebound_calls },
         { "expanded_loadbacks", result.layer_wait_patch.expanded_loadbacks },
         { "removed_main_cpu_us", result.layer_wait_patch.removed_main_cpu_us },
         { "removed_worker_us", result.layer_wait_patch.removed_worker_us },
         { "added_return_us", result.layer_wait_patch.added_return_us },
         { "inactive_call_median_ns", result.layer_wait_patch.inactive_call_median_ns } }   },
        {          "runtime_preparation",
         { { "status", preparation.status },
         { "call_counts", preparation_counts },
         { "observed_formal_calls", preparation.observed_formal_calls },
         { "blockers", preparation.blockers },
         { "changed_gap_count", preparation.mutation.set_cpu_gaps.size() },
         { "removed_coverage_us", preparation.removed_coverage_us },
         { "added_cost_us", preparation.added_cost_us },
         { "required_path_counts", required_paths },
         { "source_parallel_compilation", preparation.source_parallel_compilation },
         { "cost_samples", preparation_costs },
         { "estimate_assumption", "fresh shared disk cache; parallel first use within each batch" },
         { "cost_source", preparation.added_cost_us ? "selected_base_path_medians" : "source_prepare_load_interval_union" },
         { "coverage_is_e2e_saving", false } }                                              },
        {  "phase_duration_update_count",                 result.phase_duration_update_count },
        {   "phase_owner_conflict_count",                  result.phase_owner_conflict_count },
        {                    "component",                           result.journal.component },
        {               "mutation_count",                      result.journal.records.size() },
        {        "duration_update_count",                              duration_update_count },
        { "e2e_eligibility_update_count",                       e2e_eligibility_update_count },
        {          "active_nodes_before",                 result.journal.active_nodes_before },
        {           "active_nodes_after",                  result.journal.active_nodes_after },
        {          "active_edges_before",                 result.journal.active_edges_before },
        {           "active_edges_after",                  result.journal.active_edges_after },
        {                 "io_resources",            io_resource_result(result.io_resources) },
        {                "phase_carrier",          phase_carrier_audit(result.phase_carrier) },
        {               "topology_valid",                                     topology_valid },
        {               "blocker_counts",                              result.apply_blockers },
        {       "rewrite_counts_by_kind",       result.shadow_rewrite.counts_by_rewrite_kind },
        {                   "validation",
         Json{
         { "status", validation_ready ? "ready" : "not_ready" },
         { "decision_count", result.shadow_rewrite.decisions.size() },
         { "attributed_count", result.source_attribution.attributed_count() },
         { "rewrite_ready_count", result.shadow_rewrite.ready_count() },
         { "boundary_ready_count", result.boundary_validation.ready_count() },
         { "applied_ready_count", result.applied_validation.ready_count() },
         { "plan_journal_exact", result.applied_validation.plan_journal_exact },
         { "prospective_materialization_exact", result.applied_validation.prospective_materialization_exact },
         { "family_dependencies_exact", result.applied_validation.family_dependencies_exact },
         { "lane_dependencies_exact", result.applied_validation.lane_dependencies_exact },
         }                                                                                  },
    };
    if (result.phase_oracle_cost_replay.status != "disabled") {
        const auto & oracle = result.phase_oracle_cost_replay;
        summary["phase_oracle_cost_replay"] = Json{
            {                "status",                oracle.status },
            {   "required_cost_count",   oracle.required_cost_count },
            {   "supplied_cost_count",   oracle.supplied_cost_count },
            {    "applied_cost_count",    oracle.applied_cost_count },
            {    "oracle_duration_us",    oracle.oracle_duration_us },
            {   "applied_duration_us",   oracle.applied_duration_us },
            { "effect_identity_exact", oracle.effect_identity_exact },
            {   "target_e2e_consumed",   oracle.target_e2e_consumed },
        };
    }
    if (result.causal_timing_audit.status != "disabled_without_oracle_cost") {
        Json causal_effects = Json::array();
        for (const auto & effect : result.causal_timing_audit.effects) {
            causal_effects.push_back(Json{
                {                              "effect_id",                              effect.effect_id },
                {                            "effect_type",                            effect.effect_type },
                {                           "rewrite_kind",                           effect.rewrite_kind },
                {                       "causal_path_kind",                       effect.causal_path_kind },
                {                                 "status",                                 effect.status },
                {                 "target_cost_node_count",                 effect.target_cost_node_count },
                {                "target_cost_duration_us",                effect.target_cost_duration_us },
                {             "completion_join_node_count",             effect.completion_join_node_count },
                {                    "consumer_node_count",                    effect.consumer_node_count },
                {       "cost_node_completion_response_us",       effect.cost_node_completion_response_us },
                {      "completion_join_start_response_us",      effect.completion_join_start_response_us },
                {             "consumer_start_response_us",             effect.consumer_start_response_us },
                {     "source_completion_wait_duration_us",     effect.source_completion_wait_duration_us },
                { "source_completion_wait_gap_duration_us", effect.source_completion_wait_gap_duration_us },
                {    "source_residual_unknown_duration_us",    effect.source_residual_unknown_duration_us },
                {               "foreground_path_expected",               effect.foreground_path_expected },
                {               "completion_join_required",               effect.completion_join_required },
                {       "source_readiness_topology_reused",       effect.source_readiness_topology_reused },
                {        "source_completion_wait_blocking",        effect.source_completion_wait_blocking },
            });
        }
        summary["causal_timing_audit"] = Json{
            {                            "status",                            result.causal_timing_audit.status },
            {            "target_cost_node_count",            result.causal_timing_audit.target_cost_node_count },
            {           "target_cost_duration_us",           result.causal_timing_audit.target_cost_duration_us },
            {          "full_with_target_cost_us",          result.causal_timing_audit.full_with_target_cost_us },
            {       "full_without_target_cost_us",       result.causal_timing_audit.full_without_target_cost_us },
            {      "full_target_cost_response_us",      result.causal_timing_audit.full_target_cost_response_us },
            {       "control_with_target_cost_us",       result.causal_timing_audit.control_with_target_cost_us },
            {    "control_without_target_cost_us",    result.causal_timing_audit.control_without_target_cost_us },
            {   "control_target_cost_response_us",   result.causal_timing_audit.control_target_cost_response_us },
            { "local_cost_sensitive_effect_count", result.causal_timing_audit.local_cost_sensitive_effect_count },
            {    "local_cost_hidden_effect_count",    result.causal_timing_audit.local_cost_hidden_effect_count },
            {                           "effects",                                    std::move(causal_effects) },
            {                    "restored_exact",                    result.causal_timing_audit.restored_exact },
        };
    }
    return summary;
}
#endif

Json module_results(const std::vector<std::unique_ptr<modules::SimulationModule>> & modules) {
    Json results = Json::object();
    for (const auto & module : modules) {
        if (const auto * node_scale = dynamic_cast<const modules::node_scale::NodeScaleModule *>(module.get())) {
            results["node_scale"] = Json{
                { "scaled_nodes", node_scale->scaled_nodes() }
            };
        }
        else if (const auto * hicache = dynamic_cast<const modules::hicache::HiCacheModule *>(module.get())) {
            results["hicache"] = Json{
                { "effect_decisions", effect_decision_result(hicache->effect_decisions()) },
                {       "phase_work",            phase_work_result(hicache->phase_work()) },
            };
        }
#ifdef DEBUG
        else if (const auto * patch = dynamic_cast<const modules::hicache::HiCacheDagPatchModule *>(module.get())) {
            results["hicache_dag_patch"] = dag_patch_result(*patch);
        }
        else if (const auto * observed = dynamic_cast<const modules::hicache::HiCacheObservedPhaseCarrierModule *>(module.get())) {
            const auto & result = observed->result();
            results["hicache_observed_phase_carrier"] = Json{
                {               "status",result.status                                         },
                {       "direct_carrier",
                 Json{
                 { "status", result.direct.status },
                 { "observed_operation_count", result.direct.observed_operation_count },
                 { "canonical_prefetch_count", result.direct.canonical_prefetch_count },
                 { "synthetic_node_count", result.direct.synthetic_node_count },
                 { "dependency_count", result.direct.dependency_count },
                 { "blockers", result.direct.blockers },
                 }                                                           },
                {              "carrier", phase_carrier_audit(result.carrier) },
                {       "mutation_count",       result.journal.records.size() },
                {  "active_nodes_before",  result.journal.active_nodes_before },
                {   "active_nodes_after",   result.journal.active_nodes_after },
                {  "active_edges_before",  result.journal.active_edges_before },
                {   "active_edges_after",   result.journal.active_edges_after },
                {       "topology_valid",                result.topology.ok() },
                { "topology_issue_count",       result.topology.issues.size() },
            };
        }
#endif
    }
    return results;
}

Json run_summary(const core::DagGraph & graph, const std::vector<std::unique_ptr<modules::SimulationModule>> & modules,
                 const modules::hicache::HiCachePhaseObservationAudit & phase, bool include_source_observations, bool executing_hicache) {
    const auto stats = graph.summary_stats();
    Json root;
    root["parsed_record_count"] = graph.parsed_record_count();
    root["simulated_e2e_us"] = graph.e2e_time();
    // Execution has no target ownership projection yet. Its component fields
    // are marked unavailable below. Ordinary DAG builds do not request them.
    if (include_source_observations && !executing_hicache) {
        root["simulated_control_e2e_us"] = graph.control_e2e_time();
        root["simulated_gap_excluded_e2e_us"] = graph.gap_excluded_e2e_time();
        uint64_t scope_node_duration_us = 0;
        uint64_t scope_gap_duration_us = 0;
        size_t scope_owned_node_count = 0;
        for (const auto & node : graph.nodes()) {
            if (!node.active) continue;
            if (graph.scope_node_owned(node.id)) {
                ++scope_owned_node_count;
                scope_node_duration_us = core::checked_add_u64(scope_node_duration_us, node.duration, "scope-owned node duration exceeds uint64 range");
            }
            scope_gap_duration_us =
                core::checked_add_u64(scope_gap_duration_us, graph.scope_gap_duration(node.id), "scope-owned gap duration exceeds uint64 range");
        }
        root["scope_owned_node_count"] = scope_owned_node_count;
        root["scope_owned_node_duration_us"] = scope_node_duration_us;
        root["scope_owned_gap_duration_us"] = scope_gap_duration_us;
    }
    root["control_exclusion_interval_count"] = graph.control_exclusion_intervals().size();
    size_t blackbox_exclusion_count = 0;
    size_t snapshot_exclusion_count = 0;
    for (const auto & interval : graph.control_exclusion_intervals()) {
        if (interval.kind == core::DagControlExclusionKind::PrefillDecode) blackbox_exclusion_count++;
        else if (interval.kind == core::DagControlExclusionKind::ProfilingSnapshot) snapshot_exclusion_count++;
    }
    root["control_blackbox_exclusion_interval_count"] = blackbox_exclusion_count;
    root["control_snapshot_exclusion_interval_count"] = snapshot_exclusion_count;
#ifdef DEBUG
    root["real_e2e_us"] = graph.real_e2e_time();
    if (include_source_observations && !executing_hicache) root["gap_excluded_critical_path"] = gap_excluded_critical_path_result(graph, phase);
#endif
    root["node_count"] = stats.active_node_count;
    root["trace_node_count"] = stats.active_trace_node_count;
    root["synthetic_node_count"] = stats.active_synthetic_node_count;
    root["edge_count"] = stats.active_edge_count;
    root["stored_node_count"] = graph.node_count();
    root["stored_edge_count"] = graph.edge_count();
    root["edge_counts_by_kind"] = stats.edge_counts_by_kind;
    root["source_observations_status"] = include_source_observations ? "included" : "not_requested";
    if (include_source_observations) root["source_phase_observations"] = hicache_phase_observations(phase);
    root["module_results"] = module_results(modules);
    return root;
}

} // namespace

void write_run_summary(const std::string & filename, const core::DagGraph & graph, const std::vector<std::unique_ptr<modules::SimulationModule>> & modules,
                       const nlohmann::json & source_io_observations, const modules::hicache::HiCachePhaseObservationAudit & source_phase_observations,
                       const nlohmann::json & client_result, const modules::hicache::runtime::HiCacheWindowResult * execution,
                       bool include_source_observations) {
    auto summary = run_summary(graph, modules, source_phase_observations, include_source_observations, execution != nullptr);
    if (include_source_observations) summary["source_io_observations"] = source_io_observations;
    summary["http_client"] = client_result;
    if (execution) {
        const auto & result = *execution;
        if (include_source_observations)
            summary["module_results"]["hicache"] = {
                { "effect_decisions", effect_decision_result(result.model.effect_decisions) },
                {       "phase_work",            phase_work_result(result.model.phase_work) }
            };
        summary["module_results"]["hicache_execution"] = {
            {                             "status",                                  "executed"                                                   },
            {                      "cost_coverage",                                                                                      "partial" },
            {                            "http_us",                                                                                 result.http_us },
            {                     "prepared_facts",                                                                          result.prepared_facts },
            {                     "consumed_facts",                                                                          result.consumed_facts },
            {              "wait_template_request",                                                                   result.wait_template_request },
            {          "wait_calibration_manifest",                                                               result.wait_calibration_manifest },
            { "local_return_calibration_manifests",                                                      result.local_return_calibration_manifests },
            {         "query_calibration_manifest",                                                              result.query_calibration_manifest },
            {                 "decode_allocations",                                                                      result.decode_allocations },
            {                   "phase_admissions",                                                                        result.phase_admissions },
            {                  "changed_operators",                                                                       result.changed_operators },
            {            "source_load_submissions",                                                                 result.source_load_submissions },
            {             "completed_load_batches",                                                                  result.completed_load_batches },
            {                        "layer_calls",
             { { "prepared", result.prepared_layer_calls }, { "active", result.active_layer_calls }, { "inactive", result.inactive_layer_calls } } },
            {           "source_write_submissions",                                                                result.source_write_submissions },
            {                   "completed_writes",                                                                        result.completed_writes },
            {                "resumed_allocations",                                                                     result.resumed_allocations },
            {           "prepared_write_templates",                                                                result.prepared_write_templates },
            {         "prepared_eviction_controls",                                                              result.prepared_eviction_controls },
            {         "expanded_write_submissions",                                                              result.expanded_write_submissions },
            {            "calibrated_empty_checks",                                                                 result.calibrated_empty_checks },
            {            "generated_control_steps",                                                                 result.generated_control_steps },
            {          "generated_blocking_checks",                                                               result.generated_blocking_checks },
            { "blocking_check_cost_extrapolations",                                                      result.blocking_check_cost_extrapolations },
            {   "phase_extrapolated_control_steps",                                                        result.phase_extrapolated_control_steps },
            {      "calibrated_empty_check_cpu_us",                                                           result.calibrated_empty_check_cpu_us },
            {                  "preparation_slots",                                                                       result.preparation_slots },
            {              "executed_preparations",                                                                   result.executed_preparations },
            {                      "confirmations",
             { { "load_rounds", result.confirmations.load_rounds },
             { "write_rounds", result.confirmations.write_rounds },
             { "storage_rounds", result.confirmations.storage_rounds },
             { "replaced_rounds", result.confirmations.replaced_rounds },
             { "partial_window_rounds", result.confirmations.partial_window_rounds } }                                                            },
            {                    "cpu_collectives",
             { { "status", result.cpu_collectives.status },
             { "observed", result.cpu_collectives.observed_rounds },
             { "planned", result.cpu_collectives.planned_rounds },
             { "retained", result.cpu_collectives.source_retained_rounds } }                                                                      },
            {           "remaining_approximations",                   { "source_cpu_branch_costs", "writeback_blocking", "source_residual_waits" } }
        };
        // Full HTTP includes every retained cost. Target ownership projections
        // are not implemented yet; zero would falsely look like a valid score.
        summary["component_metrics_status"] = "unavailable_target_ownership";
        for (const auto key : { "simulated_control_e2e_us",
                                "simulated_gap_excluded_e2e_us",
                                "scope_owned_node_count",
                                "scope_owned_node_duration_us",
                                "scope_owned_gap_duration_us" })
            summary[key] = nullptr;
#ifdef DEBUG
        summary["gap_excluded_critical_path"] = nullptr;
#endif
    }
    write_json_file(filename, summary);
}

} // namespace markov::trace_graph::cli
