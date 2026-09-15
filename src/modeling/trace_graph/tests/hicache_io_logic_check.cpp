/** @file Explicit validation-build regressions; never part of profiling/prediction. */
#include "markov/trace_graph/modules/hicache/patch/io_resource_model.hpp"
#include "markov/trace_graph/modules/hicache/patch/applied_validator.hpp"
#include "markov/trace_graph/modules/hicache/patch/boundary_validator.hpp"
#include "markov/trace_graph/modules/hicache/patch/rewrite_transaction.hpp"
#include "markov/trace_graph/modules/hicache/service_model.hpp"
#include "../src/modules/hicache/patch/attribution_common.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace markov::trace_graph;
using namespace markov::trace_graph::modules::hicache;

namespace {

void require(bool value, const std::string & message) {
    if (!value) throw std::runtime_error(message);
}

void first_allocation_owns_inherited_writeback() {
    const auto fact = [](size_t id, std::string_view role, uint64_t ts, uint64_t dur, std::string_view phase,
                         bool workload = false) {
        core::TraceEvent event;
        event.index = id;
        event.source_channel = core::TraceSourceChannel::PythonProbe;
        event.name = std::string(role);
        event.pid = event.tid = "worker";
        event.ts = ts;
        event.dur = dur;
        event.set_arg("fact", "{\"class\":\"" + std::string(workload ? "workload_identity" : "source_actual")
                                  + "\",\"role\":\"" + std::string(role) + "\",\"consumers\":[\"hicache_dag_patch\"]}");
        event.set_arg("phase", phase);
        event.set_arg("target_id", role);
        event.set_arg("cache_scope", "cache");
        return event;
    };
    auto extend = fact(0, "cache_extend_input", 100, 0, "start", true);
    auto capacity = fact(1, "capacity_result_observed", 110, 30, "end");
    auto enqueue = fact(2, "commit_device_to_host_enqueue_observed", 112, 5, "end");
    enqueue.set_arg("write_back", "true");
    enqueue.set_arg("node_id", "20");
    enqueue.set_arg("effective_token_count", "512");
    enqueue.set_arg("page_hashes", "[\"page-a\",\"page-b\",\"page-c\",\"page-d\"]");
    auto release_start = fact(3, "commit_capacity_release_observed", 120, 0, "start");
    release_start.set_arg("operation_node_ids", "[20]");
    auto release_end = fact(4, "commit_capacity_release_observed", 120, 4, "end");
    auto lookup = fact(5, "cache_lookup_input", 105, 1, "end", true);
    auto lifecycle = fact(6, "cache_lifecycle_commit", 80, 5, "end", true);
    const auto claims = [](std::vector<core::TraceEvent> events, size_t owner_id) {
        core::DagGraph graph;
        graph.set_hicache_fact_events(std::move(events));
        const patch::HiCacheSourceDagIndex index(graph);
        const auto * anchor = index.fact_node(owner_id);
        require(anchor != nullptr, "fixture owner must be indexed");
        return patch::attribution_detail::commit_d2h_enqueues(index, *anchor).size();
    };
    const std::vector<core::TraceEvent> allocation{extend, capacity, enqueue, release_start, release_end};
    require(claims(allocation, 0) == 1, "first measured allocation owns its inherited dirty-page eviction");
    auto incomplete = allocation;
    incomplete.pop_back();
    require(claims(incomplete, 0) == 0, "allocation attribution still requires exact completion evidence");
    auto intervening = allocation;
    intervening.push_back(lookup);
    require(claims(intervening, 0) == 0, "a later canonical input must prevent stale allocation attribution");
    auto other_thread = allocation;
    other_thread[0].tid = "other";
    require(claims(other_thread, 0) == 0, "allocation ownership cannot cross scheduler threads");
    auto with_lifecycle = allocation;
    with_lifecycle.push_back(lifecycle);
    require(claims(with_lifecycle, 0) == 0 && claims(with_lifecycle, 6) == 1,
            "existing measured lifecycle attribution remains unique and unchanged");
}

void service_and_dag_share_batch_cost() {
    frontend::HiCacheIoServiceModelConfig service;
    service.direction = "host_to_storage";
    service.new_operation_points = {{1, 10., 1'000'000.}, {2, 10., 1'000'000.}};
    service.existing_key_bandwidth_points = {{1, 1, 1'000'000.}, {1, 4, 1'000'000.}};
    service.existing_runtime_scale = 2.;
    const auto batch = hicache_service_cost(service, 1, 2);
    require(batch && batch->duration_us == 12, "each two-byte batch pays one 10 us setup");
    const auto mixed = hicache_service_cost(service, 1, 2, 1, 1);
    require(mixed && mixed->duration_us == 13, "existing materialization is separate from new-file work");
    require(!hicache_service_cost(service, 1, 2, 1, 3), "existing pages cannot exceed executed pages");

    model::HiCacheEffectDecision decision;
    decision.effect_key = "write";
    decision.effect_type = model::HiCacheEffectType::CommitHostToStorage;
    decision.direction = model::HiCacheTransferDirection::HostToStorage;
    decision.target_effect_state = model::HiCacheTargetEffectState::Required;
    decision.cache_scope = "worker";
    decision.operation_ids = {"operation"};
    decision.effective_page_count = 4;
    decision.effective_byte_count = 4;
    decision.storage_new_page_count = 4;
    decision.storage_service_batches = {{0, 2, 0, 2}, {0, 2, 0, 2}};
    model::HiCacheEffectDecisionLedger ledger;
    ledger.byte_projection_available = true;
    ledger.kv_bytes_per_page = 1;
    ledger.decisions = {decision};
    frontend::HiCacheIoCostConfig config;
    config.service_models.emplace("write_host_to_storage", service);
    const auto plan = patch::build_hicache_io_resource_plan(ledger, config);
    require(plan.status == "ready", "two-batch resource plan must be executable");
    require(plan.costs.front().duration_us == 2 * batch->duration_us, "DAG cost equals the same per-batch service clock");
    require(plan.costs.front().duration_us == 24, "operation setup must not be charged as a per-batch setup");
}

void fixed_cost_and_resource_scope() {
    frontend::HiCacheIoServiceModelConfig service;
    service.direction = "storage_to_host";
    service.setup_us_per_operation = 10.;
    service.bandwidth_bytes_per_sec = 1'000'000.;
    service.runtime_scale = 2.;
    require(hicache_service_cost(service, 1, 0)->duration_us == 0, "no executed work has no service setup");
    require(hicache_service_cost(service, 1, 4, 2)->duration_us == 48, "fixed costs follow actual calls");
    service.direction = "host_to_device";
    service.page_bandwidth_points = {{1, 1'000'000., 10.}, {2, 1'000'000., 10.}};
    require(hicache_service_cost(service, 1, 1)->duration_us == 22, "small DMA retains setup");
    require(hicache_service_cost(service, 1, 4, 2)->duration_us == 48, "DMA setup follows operation count");
    frontend::HiCacheIoCostConfig config;
    require(hicache_resource_lane(config, "prefetch", "rank") == "rank/host_storage_read_lane", "scope-local read lane");
    config.resource_lanes.shared_storage_read = true;
    require(hicache_resource_lane(config, "prefetch", "rank") == "host_storage_read_lane", "shared read lane");
}

std::pair<size_t, size_t> policy_wait_does_not_foreground_background_service(uint64_t wait_us, uint64_t control_us) {
    core::DagGraph graph;
    const auto source = graph.add_synthetic_node({.name = "enqueue", .lane_key = "cpu", .duration = 1});
    const auto control_ready = graph.add_synthetic_node({.name = "control_ready", .lane_key = "cpu", .duration = 1});
    const auto wait_exit = graph.add_synthetic_node({.name = "wait_exit", .lane_key = "cpu", .duration = 1});
    graph.add_edge(source, control_ready, core::DagEdgeKind::Sequential);
    graph.add_edge(control_ready, wait_exit, core::DagEdgeKind::Sequential);

    model::HiCacheEffectDecision effect;
    effect.effect_key = "timeout_prefetch";
    effect.effect_family_key = "timeout_family";
    effect.effect_type = model::HiCacheEffectType::PrefetchIo;
    effect.direction = model::HiCacheTransferDirection::StorageToHost;
    effect.cache_scope = "rank";
    effect.request_id_provenance = "request";
    effect.source_execution_anchor_node_id = source;
    effect.target_effect_state = model::HiCacheTargetEffectState::Required;
    effect.consumer_dependency_required = false;
    effect.policy_wait_duration_us = wait_us;

    model::HiCacheEffectDecisionLedger effects;
    effects.decisions = {effect};

    patch::HiCacheSourceAttribution attribution;
    attribution.effect_id = effect.effect_key;
    attribution.effect_type = effect.effect_type;
    attribution.target_effect_state = effect.target_effect_state;
    attribution.source_carrier_state = model::HiCacheSourceCarrierState::Absent;
    attribution.source_execution_anchor_node_id = source;
    attribution.completion_join_contract_ready = true;
    attribution.control_ready_anchor_node_id = control_ready;
    attribution.wait_exit_anchor_node_id = wait_exit;
    attribution.terminal_control_anchor_node_id = wait_exit;
    attribution.consumer_anchors = {wait_exit};
    patch::HiCacheSourceAttributionCatalog attributions;
    attributions.records = {attribution};

    patch::HiCacheIoCostRecord cost;
    cost.effect_id = effect.effect_key;
    cost.effect_type = effect.effect_type;
    cost.direction = effect.direction;
    cost.target_effect_state = effect.target_effect_state;
    cost.duration_us = 500;
    cost.host_control_operation_count = 1;
    cost.host_control_duration_us = control_us;
    cost.resource_lane = "rank/host_storage_read_lane";
    cost.status = patch::HiCacheIoCostStatus::Ready;
    patch::HiCacheIoResourcePlan resources;
    resources.costs = {cost};

    const auto shadow = patch::build_hicache_shadow_rewrite_transaction(graph, effects, attributions, resources);
    require(shadow.status == "ready" && shadow.topology_valid, "timeout rewrite must be topology-ready");
    require(shadow.decisions.size() == 1 && shadow.decisions.front().completion_join_required == (wait_us > 0),
            "only timeout requires a foreground policy join");
    require(!shadow.decisions.front().completion_join_uses_service,
            "incomplete timeout must not make the full background service a foreground dependency");
    const auto & decision = shadow.decisions.front();
    const auto node = [&](const std::string & id) {
        return std::ranges::find_if(shadow.plan.synthetic_nodes, [&](const auto & candidate) { return candidate.synthetic_id == id; });
    };
    const auto policy_wait = node(decision.policy_wait_synthetic_id);
    require(wait_us == 0 ? policy_wait == shadow.plan.synthetic_nodes.end()
                        : policy_wait != shadow.plan.synthetic_nodes.end() && policy_wait->node.duration == wait_us,
            "timeout duration must be materialized as its own policy wait");
    const auto control = node(decision.target_host_control_synthetic_id);
    require(control != shadow.plan.synthetic_nodes.end() && control->node.duration == control_us,
            "control boundary exists independently of cost, including zero cost");
    const auto has_edge = [&](const std::string & src, const std::string & dst) {
        return std::ranges::any_of(shadow.plan.add_edges,
                                   [&](const auto & edge) { return edge.src.synthetic_id == src && edge.dst.synthetic_id == dst; });
    };
    require(has_edge(decision.policy_wait_synthetic_id, decision.completion_join_synthetic_id) == (wait_us > 0),
            "policy wait must release the completion join");
    require(!has_edge(decision.synthetic_id, decision.completion_join_synthetic_id),
            "background service must not release the timeout join");
    require(!has_edge(decision.synthetic_id, decision.target_host_control_synthetic_id),
            "background service must not become the terminal control's predecessor");

    const auto boundaries = patch::validate_hicache_shadow_boundaries(graph, shadow);
    require(boundaries.status == "ready", "timeout policy and service branches must pass boundary validation");
    const auto mutation = core::apply_dag_mutation_plan(graph, shadow.plan);
    const auto applied = patch::validate_hicache_applied_patch(graph, shadow, resources, shadow.plan, mutation, true);
    require(applied.status == "ready", "materialized timeout policy branch must pass applied validation");
    return {graph.node_count(), graph.edge_count()};
}

void oracle_preserves_terminal_control() {
    // Anonymous fixture, removed automatically when closed; no repository artifact.
    const std::unique_ptr<FILE, decltype(&std::fclose)> fixture(std::tmpfile(), std::fclose);
    require(fixture != nullptr, "oracle fixture must open");
    std::fputs(R"({"costs":[{"effect_id":"read","effect_type":"prefetch_io_operation",
        "direction":"storage_to_host","resource_scope":"rank","resource_lane":"read",
        "logical_order_epoch":0,"operation_count":1,"page_count":1,"byte_count":1,
        "service_us":500,"control_us":3}]})", fixture.get());
    std::fflush(fixture.get());
    patch::HiCacheIoCostRecord cost;
    cost.effect_id = "read";
    cost.effect_type = model::HiCacheEffectType::PrefetchIo;
    cost.direction = model::HiCacheTransferDirection::StorageToHost;
    cost.resource_scope = "rank";
    cost.resource_lane = "read";
    cost.operation_count = cost.host_control_operation_count = cost.effective_byte_count = 1;
    cost.duration_us = 500;
    cost.host_control_duration_us = 3;
    cost.status = patch::HiCacheIoCostStatus::Ready;
    patch::HiCacheIoResourcePlan plan;
    plan.kv_bytes_per_page = 1;
    plan.costs = {cost};
    patch::apply_hicache_oracle_cost_replay(plan, "/proc/self/fd/" + std::to_string(::fileno(fixture.get())));
    require(plan.costs.front().duration_us == 500 && plan.costs.front().host_control_duration_us == 3,
            "identical oracle costs must retain payload service and terminal control");
    require(plan.oracle_cost_replay.applied_control_us == plan.oracle_cost_replay.oracle_control_us,
            "all observed intrinsic control is applied");
}

} // namespace

int main() {
    first_allocation_owns_inherited_writeback();
    service_and_dag_share_batch_cost();
    fixed_cost_and_resource_scope();
    oracle_preserves_terminal_control();
    for (uint64_t wait_us : {0, 100}) {
        require(policy_wait_does_not_foreground_background_service(wait_us, 0)
                    == policy_wait_does_not_foreground_background_service(wait_us, 3),
                "changing control cost cannot change node/edge counts");
    }
    std::cout << "HiCache I/O logic checks passed\n";
}
