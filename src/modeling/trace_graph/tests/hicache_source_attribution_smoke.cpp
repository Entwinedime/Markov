/**
 * @file
 * @brief Concentrated identity-contract smoke test for HiCache source attribution.
 */
#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"
#include "markov/trace_graph/modules/hicache/patch/attribution.hpp"

#include <algorithm>
#include <initializer_list>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using markov::trace_graph::core::DagEdgeKind;
using markov::trace_graph::core::DagGraph;
using markov::trace_graph::core::TraceEvent;
using markov::trace_graph::core::TraceSourceChannel;
using markov::trace_graph::modules::hicache::HiCacheDagPatchModule;
using markov::trace_graph::modules::hicache::model::HiCacheEffectDecision;
using markov::trace_graph::modules::hicache::model::HiCacheEffectDecisionLedger;
using markov::trace_graph::modules::hicache::model::HiCacheEffectType;
using markov::trace_graph::modules::hicache::model::HiCacheModelResult;
using markov::trace_graph::modules::hicache::model::HiCacheSourceCarrierState;
using markov::trace_graph::modules::hicache::model::HiCacheTargetEffectState;
using markov::trace_graph::modules::hicache::model::HiCacheTransferDirection;
using markov::trace_graph::modules::hicache::patch::build_hicache_source_attribution;
using markov::trace_graph::modules::hicache::patch::HiCacheSourceDagIndex;
using markov::trace_graph::modules::hicache::patch::validate_hicache_applied_patch;

void expect(bool condition, const std::string & message) {
    if (!condition) throw std::runtime_error(message);
}

TraceEvent fact_event(size_t index, std::string_view fact_class, std::string_view role, uint64_t timestamp_us, uint64_t duration_us,
                      std::initializer_list<std::pair<std::string_view, std::string_view>> args = {}) {
    TraceEvent event;
    event.index = index;
    event.source_channel = TraceSourceChannel::PythonProbe;
    event.name = "hicache." + std::string(role);
    event.cat = "python_probe";
    event.ts = timestamp_us;
    event.dur = duration_us;
    event.pid = "worker-0";
    event.tid = "thread-0";
    event.set_arg("fact", "{\"class\":\"" + std::string(fact_class) + "\",\"role\":\"" + std::string(role) + "\",\"consumers\":[\"hicache_dag_patch\"]}");
    for (const auto & [key, value] : args) event.set_arg(key, value);
    return event;
}

DagGraph source_graph() {
    std::vector<TraceEvent> events;
    auto append = [&](std::string_view fact_class,
                      std::string_view role,
                      uint64_t timestamp_us,
                      uint64_t duration_us,
                      std::initializer_list<std::pair<std::string_view, std::string_view>> args = {}) {
        events.push_back(fact_event(events.size(), fact_class, role, timestamp_us, duration_us, args));
    };

    append("workload_identity",
           "cache_lookup_input",
           10,
           1,
           {
               { "request_id", "loadback-request" }
    });
    append("source_actual",
           "loadback_decision_observed",
           20,
           1,
           {
               {            "request_id", "loadback-request" },
               { "effective_token_count",              "128" },
               {               "node_id",               "42" }
    });
    append("timing_observation",
           "loadback_io_observed",
           30,
           0,
           {
               {              "phase",         "start" },
               {          "target_id", "loadback-call" },
               { "operation_node_ids",          "[42]" }
    });
    append("timing_observation",
           "loadback_io_observed",
           30,
           25,
           {
               {     "phase",           "end" },
               { "target_id", "loadback-call" }
    });

    append("workload_identity",
           "cache_lifecycle_commit",
           100,
           400,
           {
               { "request_id", "commit-request" }
    });
    append("source_actual",
           "commit_device_to_host_enqueue_observed",
           120,
           1,
           {
               {                 "phase", "end" },
               { "effective_token_count",  "64" },
               {               "node_id",  "77" }
    });
    append("source_actual",
           "commit_capacity_release_observed",
           200,
           0,
           {
               {              "phase",        "start" },
               {          "target_id", "release-call" },
               { "operation_node_ids",         "[77]" }
    });
    append("source_actual",
           "commit_capacity_release_observed",
           200,
           150,
           {
               {     "phase",          "end" },
               { "target_id", "release-call" }
    });
    append("source_actual",
           "writeback_enqueue_observed",
           220,
           1,
           {
               {        "phase",                 "end" },
               { "operation_id", "storage-operation-9" }
    });
    append("timing_observation",
           "writeback_io_observed",
           240,
           40,
           {
               {        "phase",                 "end" },
               { "operation_id", "storage-operation-9" }
    });

    append("workload_identity",
           "prefetch_candidate_anchor",
           400,
           1,
           {
               { "request_id", "prefetch-request" }
    });
    append("timing_observation",
           "prefetch_io_observed",
           401,
           50,
           {
               {   "request_id",    "different-request" },
               { "operation_id", "nearby-but-unrelated" }
    });

    DagGraph graph(std::move(events), 0);
    for (size_t event_index = 0; event_index < graph.events().size(); ++event_index) graph.add_node(event_index, true, "worker-0/thread-0");
    for (size_t node_id = 1; node_id < graph.node_count(); ++node_id) graph.add_edge(node_id - 1, node_id, DagEdgeKind::Sequential);
    return graph;
}

HiCacheEffectDecision decision(std::string effect_id, HiCacheEffectType effect_type, size_t source_node_id, std::string source_role,
                               std::string request_id = {}) {
    return HiCacheEffectDecision{
        .effect_key = std::move(effect_id),
        .effect_type = effect_type,
        .cache_scope = "scope:1",
        .request_id_provenance = std::move(request_id),
        .source_fact_role = std::move(source_role),
        .source_node_id = source_node_id,
        .target_effect_state = HiCacheTargetEffectState::Required,
    };
}

void check_exact_identity_chains() {
    const auto graph = source_graph();
    const HiCacheSourceDagIndex source(graph);
    expect(source.stats().status == "ready", "source index rejected valid probe facts");

    HiCacheEffectDecisionLedger ledger;
    ledger.decisions = {
        decision("loadback", HiCacheEffectType::Loadback, 0, "cache_lookup_input", "loadback-request"),
        decision("commit-h2s", HiCacheEffectType::CommitHostToStorage, 4, "cache_lifecycle_commit"),
        decision("prefetch-nearby", HiCacheEffectType::PrefetchIo, 10, "prefetch_candidate_anchor", "prefetch-request"),
    };
    const auto catalog = build_hicache_source_attribution(source, ledger);
    expect(catalog.status == "ready", "exact carriers and one contract-proven absence did not produce a ready catalog");
    expect(catalog.records.size() == 3, "source attribution dropped an effect decision");

    const auto & loadback = catalog.records[0];
    expect(loadback.source_carrier_state == HiCacheSourceCarrierState::Present, "exact loadback tree-node call was not attributed");
    expect(loadback.identity_method == "request_id+pid+opportunity_window+tree_node_id+call_identity", "loadback identity method changed");
    expect(loadback.carrier_nodes == std::vector<size_t>{ 3 }, "loadback did not retain the paired call-end duration node");

    const auto & commit = catalog.records[1];
    expect(commit.source_carrier_state == HiCacheSourceCarrierState::Present, "exact commit storage operation was not attributed");
    expect(commit.identity_method == "tree_node_id+ack_call_containment+operation_id+pid", "commit H2S identity method changed");
    expect(commit.carrier_nodes == std::vector<size_t>{ 9 }, "commit H2S did not join enqueue and timing by operation ID");

    const auto & nearby = catalog.records[2];
    expect(nearby.source_carrier_state == HiCacheSourceCarrierState::Absent, "exact-request contract did not distinguish absence from nearby unrelated timing");
    expect(nearby.timing_fact_nodes.empty(), "request mismatch leaked a timing candidate into attribution");
}

void check_repeated_lookup_opportunity_window() {
    std::vector<TraceEvent> events;
    events.push_back(fact_event(0,
                                "workload_identity",
                                "cache_lookup_input",
                                10,
                                1,
                                {
                                    { "request_id", "repeated-lookup" }
    }));
    events.push_back(fact_event(1,
                                "workload_identity",
                                "cache_lookup_input",
                                20,
                                1,
                                {
                                    { "request_id", "repeated-lookup" }
    }));
    events.push_back(fact_event(2,
                                "source_actual",
                                "loadback_decision_observed",
                                30,
                                1,
                                {
                                    {            "request_id", "repeated-lookup" },
                                    { "effective_token_count",             "128" },
                                    {               "node_id",              "42" }
    }));
    events.push_back(fact_event(3,
                                "timing_observation",
                                "loadback_io_observed",
                                40,
                                0,
                                {
                                    {              "phase",                  "start" },
                                    {          "target_id", "repeated-loadback-call" },
                                    { "operation_node_ids",                   "[42]" }
    }));
    events.push_back(fact_event(4,
                                "timing_observation",
                                "loadback_io_observed",
                                40,
                                25,
                                {
                                    {     "phase",                    "end" },
                                    { "target_id", "repeated-loadback-call" }
    }));
    DagGraph graph(std::move(events), 0);
    for (size_t event_index = 0; event_index < graph.events().size(); ++event_index) graph.add_node(event_index, true, "cpu");
    for (size_t node_id = 1; node_id < graph.node_count(); ++node_id) graph.add_edge(node_id - 1, node_id, DagEdgeKind::Sequential);

    HiCacheEffectDecisionLedger ledger;
    ledger.decisions = {
        decision("earlier-lookup", HiCacheEffectType::Loadback, 0, "cache_lookup_input", "repeated-lookup"),
        decision("latest-lookup", HiCacheEffectType::Loadback, 1, "cache_lookup_input", "repeated-lookup"),
    };
    const HiCacheSourceDagIndex source(graph);
    const auto catalog = build_hicache_source_attribution(source, ledger);
    expect(catalog.status == "ready", "repeated lookup window did not produce complete attribution");
    expect(catalog.records[0].source_carrier_state == HiCacheSourceCarrierState::Absent, "an earlier lookup claimed a later lookup's loadback operation");
    expect(catalog.records[1].source_carrier_state == HiCacheSourceCarrierState::Present, "the latest lookup did not claim its loadback operation");
    expect(catalog.records[1].carrier_nodes == std::vector<size_t>{ 4 }, "the latest lookup selected the wrong loadback timing carrier");
}

DagGraph contract_absence_graph() {
    std::vector<TraceEvent> events;
    events.push_back(fact_event(0,
                                "workload_identity",
                                "cache_lookup_input",
                                10,
                                1,
                                {
                                    { "request_id", "contract-loadback" }
    }));
    events.push_back(fact_event(1,
                                "source_actual",
                                "request_admission_observed",
                                20,
                                1,
                                {
                                    { "request_id", "contract-loadback" }
    }));
    events.push_back(fact_event(2,
                                "workload_identity",
                                "cache_lifecycle_commit",
                                30,
                                10,
                                {
                                    { "request_id", "contract-commit" }
    }));
    events.push_back(fact_event(3,
                                "workload_identity",
                                "prefetch_candidate_anchor",
                                50,
                                1,
                                {
                                    { "request_id", "next-consumer" }
    }));
    DagGraph graph(std::move(events), 0);
    for (size_t event_index = 0; event_index < graph.events().size(); ++event_index) graph.add_node(event_index, true, "cpu");
    graph.add_edge(0, 1, DagEdgeKind::Sequential);
    graph.add_edge(2, 3, DagEdgeKind::Sequential);
    return graph;
}

HiCacheEffectDecisionLedger contract_absence_ledger() {
    HiCacheEffectDecisionLedger ledger;
    ledger.status = "ready";
    auto loadback = decision("contract-loadback", HiCacheEffectType::Loadback, 0, "cache_lookup_input", "contract-loadback");
    loadback.target_effect_state = HiCacheTargetEffectState::NotRequired;
    auto d2h = decision("contract-d2h", HiCacheEffectType::CommitDeviceToHost, 2, "cache_lifecycle_commit");
    d2h.target_effect_state = HiCacheTargetEffectState::NotRequired;
    auto h2s = decision("contract-h2s", HiCacheEffectType::CommitHostToStorage, 2, "cache_lifecycle_commit");
    h2s.target_effect_state = HiCacheTargetEffectState::NotRequired;
    auto capacity = decision("contract-capacity", HiCacheEffectType::CommitCapacityGate, 2, "cache_lifecycle_commit");
    capacity.target_effect_state = HiCacheTargetEffectState::NotRequired;
    ledger.decisions = { std::move(loadback), std::move(d2h), std::move(h2s), std::move(capacity) };
    return ledger;
}

void check_manifest_contract_absence() {
    {
        auto graph = contract_absence_graph();
        graph.set_input_contracts({ "hicache_dag_patch" });
        const HiCacheSourceDagIndex source(graph);
        expect(source.stats().dag_patch_contract_ready, "manifest DAG-patch contract was not indexed");
        const auto catalog = build_hicache_source_attribution(source, contract_absence_ledger());
        expect(catalog.status == "ready", "configured zero-event roles did not produce ready absence attribution");
        expect(std::ranges::all_of(catalog.records, [](const auto & record) { return record.source_carrier_state == HiCacheSourceCarrierState::Absent; }),
               "configured zero-event role was confused with an unavailable probe contract");
    }
    {
        auto graph = contract_absence_graph();
        const HiCacheSourceDagIndex source(graph);
        expect(!source.stats().dag_patch_contract_ready, "graph invented a manifest DAG-patch contract");
        const auto catalog = build_hicache_source_attribution(source, contract_absence_ledger());
        expect(catalog.status == "blocked", "missing probe contract was guessed as explicit source absence");
        expect(std::ranges::all_of(catalog.records, [](const auto & record) { return record.source_carrier_state == HiCacheSourceCarrierState::Unobservable; }),
               "missing probe contract did not preserve unobservable source state");
    }
}

DagGraph prefetch_patch_graph(std::string timing_request_id = "patch-request") {
    std::vector<TraceEvent> events;
    events.push_back(fact_event(0,
                                "workload_identity",
                                "prefetch_candidate_anchor",
                                10,
                                1,
                                {
                                    { "request_id", "patch-request" }
    }));
    events.push_back(fact_event(1,
                                "timing_observation",
                                "prefetch_io_observed",
                                20,
                                25,
                                {
                                    {   "request_id",    timing_request_id },
                                    { "operation_id", "prefetch-operation" }
    }));
    events.push_back(fact_event(2,
                                "source_actual",
                                "request_admission_observed",
                                50,
                                1,
                                {
                                    { "request_id", "patch-request" }
    }));
    DagGraph graph(std::move(events), 0);
    graph.add_node(0, true, "cpu");
    graph.add_node(1, true, "cpu");
    graph.add_node(2, true, "cpu");
    graph.add_edge(0, 1, DagEdgeKind::Sequential);
    graph.add_edge(1, 2, DagEdgeKind::Sequential);
    return graph;
}

std::shared_ptr<HiCacheModelResult> patch_model_result(std::string calibration_status, std::string request_id = "patch-request", size_t consumer_node_id = 2) {
    auto result = std::make_shared<HiCacheModelResult>();
    result->replay_complete = true;
    auto & ledger = result->effect_decisions;
    ledger.status = "ready";
    ledger.kv_bytes_per_page = 100;
    ledger.byte_projection_available = true;
    ledger.byte_projection_source = "smoke";
    ledger.io_model_id = "source-attribution-smoke";
    ledger.io_model_digest = "sha256_json:source-attribution-smoke";
    ledger.io_model_calibration_status = std::move(calibration_status);
    ledger.resource_model = "scope_local_directional_device_host_shared_host_storage_v1";
    ledger.device_host_bandwidth_bytes_per_sec = 100;
    ledger.host_storage_bandwidth_bytes_per_sec = 100;
    ledger.io_model_provenance = {
        {  "device_host_bandwidth", "smoke" },
        { "host_storage_bandwidth", "smoke" },
        {            "kv_geometry", "smoke" },
    };
    auto effect = decision("prefetch-patch", HiCacheEffectType::PrefetchIo, 0, "prefetch_candidate_anchor", std::move(request_id));
    effect.direction = HiCacheTransferDirection::StorageToHost;
    effect.candidate_page_count = 1;
    effect.effective_page_count = 1;
    effect.effective_byte_count = 100;
    effect.eligibility_boundary = { .kind = "prefetch_candidate", .epoch = 1, .timestamp_us = 10 };
    effect.consumer_boundary = {
        .kind = "request_admission_consumer",
        .epoch = 2,
        .timestamp_us = 50,
        .source_node_id = consumer_node_id,
        .source_event_index = consumer_node_id,
        .source_fact_role = "request_admission_observed",
    };
    ledger.decisions.push_back(std::move(effect));
    return result;
}

std::shared_ptr<HiCacheModelResult> dependency_model_result(HiCacheEffectDecision effect) {
    auto result = std::make_shared<HiCacheModelResult>();
    result->replay_complete = true;
    auto & ledger = result->effect_decisions;
    ledger.status = "ready";
    ledger.kv_bytes_per_page = 100;
    ledger.byte_projection_available = true;
    ledger.byte_projection_source = "smoke";
    ledger.io_model_id = "source-attribution-smoke";
    ledger.io_model_digest = "sha256_json:source-attribution-smoke";
    ledger.io_model_calibration_status = "calibrated";
    ledger.resource_model = "scope_local_directional_device_host_shared_host_storage_v1";
    ledger.device_host_bandwidth_bytes_per_sec = 100;
    ledger.host_storage_bandwidth_bytes_per_sec = 100;
    ledger.io_model_provenance = {
        {  "device_host_bandwidth", "smoke" },
        { "host_storage_bandwidth", "smoke" },
        {            "kv_geometry", "smoke" },
    };
    ledger.decisions.push_back(std::move(effect));
    return result;
}

void check_dependency_gate_ownership() {
    {
        std::vector<TraceEvent> events;
        events.push_back(fact_event(0,
                                    "workload_identity",
                                    "prefetch_candidate_anchor",
                                    10,
                                    1,
                                    {
                                        { "request_id", "no-visibility-request" }
        }));
        DagGraph graph(std::move(events), 0);
        graph.add_node(0, true, "cpu");

        HiCacheEffectDecisionLedger ledger;
        ledger.status = "ready";
        auto effect = decision("no-visibility", HiCacheEffectType::PrefetchVisibility, 0, "prefetch_candidate_anchor", "no-visibility-request");
        effect.target_effect_state = HiCacheTargetEffectState::NotRequired;
        ledger.decisions.push_back(std::move(effect));

        const HiCacheSourceDagIndex source(graph);
        const auto catalog = build_hicache_source_attribution(source, ledger);
        expect(catalog.status == "ready", "source/target visibility no-op did not produce a ready attribution catalog");
        expect(catalog.records[0].source_carrier_state == HiCacheSourceCarrierState::Absent,
               "visibility no-op incorrectly required a target consumer boundary");
        expect(catalog.records[0].consumer_anchors.empty(), "visibility no-op invented a consumer boundary");
    }
    {
        std::vector<TraceEvent> events;
        events.push_back(fact_event(0,
                                    "workload_identity",
                                    "prefetch_candidate_anchor",
                                    10,
                                    1,
                                    {
                                        { "request_id", "ready-progress-request" }
        }));
        events.push_back(fact_event(1,
                                    "source_actual",
                                    "prefetch_intent_observed",
                                    20,
                                    1,
                                    {
                                        { "request_id", "ready-progress-request" }
        }));
        events.push_back(fact_event(2,
                                    "source_actual",
                                    "prefetch_progress_observed",
                                    30,
                                    1,
                                    {
                                        {     "request_id", "ready-progress-request" },
                                        { "progress_ready",                   "true" }
        }));
        DagGraph graph(std::move(events), 0);
        for (size_t event_index = 0; event_index < graph.events().size(); ++event_index) graph.add_node(event_index, true, "cpu");

        HiCacheEffectDecisionLedger ledger;
        ledger.status = "ready";
        auto effect = decision("ready-progress", HiCacheEffectType::PrefetchVisibility, 0, "prefetch_candidate_anchor", "ready-progress-request");
        effect.target_effect_state = HiCacheTargetEffectState::NotRequired;
        ledger.decisions.push_back(std::move(effect));

        const HiCacheSourceDagIndex source(graph);
        const auto catalog = build_hicache_source_attribution(source, ledger);
        expect(catalog.status == "ready", "non-blocking progress result did not produce a ready attribution");
        expect(catalog.records[0].source_carrier_state == HiCacheSourceCarrierState::Absent,
               "progress_ready=true was incorrectly classified as a source visibility dependency");
        expect(catalog.records[0].consumer_anchors.empty(), "non-blocking progress invented a consumer dependency");
    }
    {
        std::vector<TraceEvent> events;
        events.push_back(fact_event(0,
                                    "workload_identity",
                                    "prefetch_candidate_anchor",
                                    10,
                                    1,
                                    {
                                        { "request_id", "unknown-progress-request" }
        }));
        events.push_back(fact_event(1,
                                    "source_actual",
                                    "prefetch_intent_observed",
                                    20,
                                    1,
                                    {
                                        { "request_id", "unknown-progress-request" }
        }));
        events.push_back(fact_event(2,
                                    "source_actual",
                                    "prefetch_progress_observed",
                                    30,
                                    1,
                                    {
                                        { "request_id", "unknown-progress-request" }
        }));
        events.push_back(fact_event(3,
                                    "workload_identity",
                                    "cache_extend_input",
                                    40,
                                    1,
                                    {
                                        { "batch_request_ids", "[\"unknown-progress-request\"]" }
        }));
        DagGraph graph(std::move(events), 0);
        for (size_t event_index = 0; event_index < graph.events().size(); ++event_index) graph.add_node(event_index, true, "cpu");
        for (size_t node_id = 1; node_id < graph.node_count(); ++node_id) graph.add_edge(node_id - 1, node_id, DagEdgeKind::Sequential);

        HiCacheEffectDecisionLedger ledger;
        ledger.status = "ready";
        auto effect = decision("unknown-progress", HiCacheEffectType::PrefetchVisibility, 0, "prefetch_candidate_anchor", "unknown-progress-request");
        effect.consumer_boundary = {
            .kind = "cache_extend_consumer",
            .epoch = 1,
            .timestamp_us = 40,
            .source_node_id = 3,
            .source_event_index = 3,
            .source_fact_role = "cache_extend_input",
        };
        ledger.decisions.push_back(std::move(effect));

        const HiCacheSourceDagIndex source(graph);
        const auto catalog = build_hicache_source_attribution(source, ledger);
        expect(catalog.status == "blocked", "missing progress result did not block source attribution");
        expect(catalog.records[0].source_carrier_state == HiCacheSourceCarrierState::Unobservable,
               "missing progress_ready was guessed as a source visibility dependency");
    }
    {
        std::vector<TraceEvent> events;
        events.push_back(fact_event(0,
                                    "workload_identity",
                                    "prefetch_candidate_anchor",
                                    10,
                                    1,
                                    {
                                        { "request_id", "visibility-request" }
        }));
        events.push_back(fact_event(1,
                                    "source_actual",
                                    "prefetch_progress_observed",
                                    20,
                                    7,
                                    {
                                        {     "request_id", "visibility-request" },
                                        { "progress_ready",              "false" }
        }));
        events.push_back(fact_event(2,
                                    "workload_identity",
                                    "cache_extend_input",
                                    40,
                                    3,
                                    {
                                        { "batch_request_ids", "[\"visibility-request\"]" }
        }));
        DagGraph graph(std::move(events), 0);
        graph.add_node(0, true, "cpu");
        graph.add_node(1, true, "cpu");
        graph.add_node(2, true, "cpu");
        graph.add_edge(0, 1, DagEdgeKind::Sequential);
        graph.add_edge(1, 2, DagEdgeKind::Sequential);

        const HiCacheSourceDagIndex source(graph);
        expect(source.nodes_for_request("visibility-request").size() == 3, "batch cache-extend request identity was not indexed");
        expect(source.fact_node(1)->progress_ready == false, "prefetch progress result was not parsed");

        auto effect = decision("visibility", HiCacheEffectType::PrefetchVisibility, 0, "prefetch_candidate_anchor", "visibility-request");
        effect.consumer_boundary = {
            .kind = "cache_extend_consumer",
            .epoch = 1,
            .timestamp_us = 40,
            .source_node_id = 2,
            .source_event_index = 2,
            .source_fact_role = "cache_extend_input",
        };
        HiCacheDagPatchModule module(dependency_model_result(std::move(effect)));
        module.apply(graph);
        const auto & patch = module.result();
        expect(patch.status == "applied", "complete prefetch visibility gate was not applied");
        expect(patch.source_attribution.records[0].owned_duration_nodes.empty(), "prefetch visibility incorrectly claimed scheduler CPU duration");
        expect(graph.node(1).duration == 7, "prefetch visibility gate changed progress-check duration");
        expect(graph.node_count() == 4 && graph.active_edge_count() == 4, "prefetch visibility gate produced the wrong active graph shape");
    }
    {
        auto graph = source_graph();
        auto effect = decision("capacity", HiCacheEffectType::CommitCapacityGate, 4, "cache_lifecycle_commit");
        effect.consumer_boundary = {
            .kind = "host_capacity_consumer",
            .epoch = 2,
            .timestamp_us = 400,
            .source_node_id = 10,
            .source_event_index = 10,
            .source_fact_role = "prefetch_candidate_anchor",
        };
        HiCacheDagPatchModule module(dependency_model_result(std::move(effect)));
        module.apply(graph);
        const auto & patch = module.result();
        expect(patch.status == "applied", "complete commit capacity gate was not applied");
        expect(patch.source_attribution.records[0].owned_duration_nodes.empty(), "capacity gate incorrectly claimed ACK/control duration");
        expect(graph.node(7).duration == 150, "capacity gate changed release-call duration");
    }
}

void check_prefetch_family_dependency() {
    std::vector<TraceEvent> events;
    events.push_back(fact_event(0,
                                "workload_identity",
                                "prefetch_candidate_anchor",
                                10,
                                1,
                                {
                                    { "request_id", "family-request" }
    }));
    events.push_back(fact_event(1,
                                "timing_observation",
                                "prefetch_io_observed",
                                20,
                                25,
                                {
                                    {   "request_id",   "family-request" },
                                    { "operation_id", "family-operation" }
    }));
    events.push_back(fact_event(2,
                                "source_actual",
                                "prefetch_progress_observed",
                                30,
                                7,
                                {
                                    {     "request_id", "family-request" },
                                    { "progress_ready",           "true" }
    }));
    events.push_back(fact_event(3,
                                "workload_identity",
                                "cache_extend_input",
                                40,
                                3,
                                {
                                    { "batch_request_ids", "[\"family-request\"]" }
    }));
    DagGraph graph(std::move(events), 0);
    for (size_t event_index = 0; event_index < graph.events().size(); ++event_index) graph.add_node(event_index, true, "cpu");
    for (size_t node_id = 1; node_id < graph.node_count(); ++node_id) graph.add_edge(node_id - 1, node_id, DagEdgeKind::Sequential);

    auto result = dependency_model_result(HiCacheEffectDecision{});
    result->effect_decisions.decisions.clear();
    auto io = decision("family-io", HiCacheEffectType::PrefetchIo, 0, "prefetch_candidate_anchor", "family-request");
    io.effect_family_key = "prefetch-family";
    io.direction = HiCacheTransferDirection::StorageToHost;
    io.candidate_page_count = 1;
    io.effective_page_count = 1;
    io.effective_byte_count = 100;
    io.consumer_boundary = {
        .kind = "prefetch_visibility_consumer",
        .epoch = 1,
        .timestamp_us = 40,
        .source_node_id = 3,
        .source_event_index = 3,
        .source_fact_role = "cache_extend_input",
    };
    auto visibility = decision("family-visibility", HiCacheEffectType::PrefetchVisibility, 0, "prefetch_candidate_anchor", "family-request");
    visibility.effect_family_key = "prefetch-family";
    visibility.consumer_boundary = io.consumer_boundary;
    result->effect_decisions.decisions = { std::move(io), std::move(visibility) };

    HiCacheDagPatchModule module(std::move(result));
    module.apply(graph);
    const auto & patch = module.result();
    expect(patch.status == "applied", "complete prefetch family transaction was not applied");
    std::map<std::string, size_t> synthetic_nodes;
    for (const auto & record : patch.journal.records) {
        if (record.action == markov::trace_graph::core::DagMutationAction::AddSyntheticNode && record.node_id)
            synthetic_nodes.emplace(record.effect_id, *record.node_id);
    }
    expect(synthetic_nodes.contains("family-io") && synthetic_nodes.contains("family-visibility"), "prefetch family synthetic endpoints are missing");
    expect(graph.has_active_edge(synthetic_nodes.at("family-io"), synthetic_nodes.at("family-visibility"), DagEdgeKind::Mutation, "hicache_family_dependency"),
           "prefetch I/O does not precede its visibility gate");
}

void check_commit_family_and_resource_dependencies() {
    std::vector<TraceEvent> events;
    events.push_back(fact_event(0,
                                "workload_identity",
                                "prefetch_candidate_anchor",
                                10,
                                1,
                                {
                                    { "request_id", "earlier-prefetch" }
    }));
    events.push_back(fact_event(1,
                                "timing_observation",
                                "prefetch_io_observed",
                                20,
                                10,
                                {
                                    {   "request_id",   "earlier-prefetch" },
                                    { "operation_id", "prefetch-operation" }
    }));
    events.push_back(fact_event(2,
                                "source_actual",
                                "request_admission_observed",
                                40,
                                1,
                                {
                                    { "request_id", "earlier-prefetch" }
    }));
    events.push_back(fact_event(3,
                                "workload_identity",
                                "cache_lifecycle_commit",
                                100,
                                400,
                                {
                                    { "request_id", "commit-family-request" }
    }));
    events.push_back(fact_event(4,
                                "source_actual",
                                "commit_device_to_host_enqueue_observed",
                                120,
                                1,
                                {
                                    {                 "phase", "end" },
                                    { "effective_token_count",  "64" },
                                    {               "node_id",  "77" }
    }));
    events.push_back(fact_event(5,
                                "timing_observation",
                                "commit_device_to_host_io_observed",
                                130,
                                0,
                                {
                                    {              "phase",    "start" },
                                    {          "target_id", "d2h-call" },
                                    { "operation_node_ids",     "[77]" }
    }));
    events.push_back(fact_event(6,
                                "timing_observation",
                                "commit_device_to_host_io_observed",
                                130,
                                30,
                                {
                                    {     "phase",      "end" },
                                    { "target_id", "d2h-call" }
    }));
    events.push_back(fact_event(7,
                                "source_actual",
                                "commit_capacity_release_observed",
                                200,
                                0,
                                {
                                    {              "phase",        "start" },
                                    {          "target_id", "release-call" },
                                    { "operation_node_ids",         "[77]" }
    }));
    events.push_back(fact_event(8,
                                "source_actual",
                                "commit_capacity_release_observed",
                                200,
                                150,
                                {
                                    {     "phase",          "end" },
                                    { "target_id", "release-call" }
    }));
    events.push_back(fact_event(9,
                                "source_actual",
                                "writeback_enqueue_observed",
                                220,
                                1,
                                {
                                    {        "phase",               "end" },
                                    { "operation_id", "storage-operation" }
    }));
    events.push_back(fact_event(10,
                                "timing_observation",
                                "writeback_io_observed",
                                240,
                                40,
                                {
                                    {        "phase",               "end" },
                                    { "operation_id", "storage-operation" }
    }));
    events.push_back(fact_event(11,
                                "workload_identity",
                                "prefetch_candidate_anchor",
                                510,
                                1,
                                {
                                    { "request_id", "next-capacity-consumer" }
    }));

    DagGraph graph(std::move(events), 0);
    for (size_t event_index = 0; event_index < graph.events().size(); ++event_index) graph.add_node(event_index, true, "cpu");
    for (size_t node_id = 1; node_id < graph.node_count(); ++node_id) graph.add_edge(node_id - 1, node_id, DagEdgeKind::Sequential);

    auto result = dependency_model_result(HiCacheEffectDecision{});
    result->effect_decisions.decisions.clear();

    auto prefetch = decision("earlier-prefetch-io", HiCacheEffectType::PrefetchIo, 0, "prefetch_candidate_anchor", "earlier-prefetch");
    prefetch.effect_family_key = "earlier-prefetch-family";
    prefetch.direction = HiCacheTransferDirection::StorageToHost;
    prefetch.effective_page_count = 1;
    prefetch.effective_byte_count = 100;
    prefetch.eligibility_boundary = { .kind = "prefetch_candidate", .epoch = 1, .timestamp_us = 10 };
    prefetch.consumer_boundary = {
        .kind = "request_admission_consumer",
        .epoch = 1,
        .timestamp_us = 40,
        .source_node_id = 2,
        .source_event_index = 2,
        .source_fact_role = "request_admission_observed",
    };

    auto d2h = decision("commit-d2h", HiCacheEffectType::CommitDeviceToHost, 3, "cache_lifecycle_commit");
    d2h.effect_family_key = "commit-family";
    d2h.direction = HiCacheTransferDirection::DeviceToHost;
    d2h.effective_page_count = 1;
    d2h.effective_byte_count = 100;
    d2h.eligibility_boundary = { .kind = "commit", .epoch = 2, .timestamp_us = 100 };

    auto h2s = decision("commit-h2s", HiCacheEffectType::CommitHostToStorage, 3, "cache_lifecycle_commit");
    h2s.effect_family_key = "commit-family";
    h2s.direction = HiCacheTransferDirection::HostToStorage;
    h2s.effective_page_count = 1;
    h2s.effective_byte_count = 100;
    h2s.eligibility_boundary = { .kind = "commit", .epoch = 3, .timestamp_us = 100 };

    auto capacity = decision("commit-capacity", HiCacheEffectType::CommitCapacityGate, 3, "cache_lifecycle_commit");
    capacity.effect_family_key = "commit-family";
    capacity.consumer_boundary = {
        .kind = "host_capacity_consumer",
        .epoch = 4,
        .timestamp_us = 510,
        .source_node_id = 11,
        .source_event_index = 11,
        .source_fact_role = "prefetch_candidate_anchor",
    };
    result->effect_decisions.decisions = { std::move(prefetch), std::move(d2h), std::move(h2s), std::move(capacity) };

    HiCacheDagPatchModule module(std::move(result));
    module.apply(graph);
    const auto & patch = module.result();
    expect(patch.status == "applied", "complete commit family transaction was not applied");
    expect(patch.applied_validation.status == "ready", "commit family failed post-apply semantic validation");
    expect(patch.applied_validation.family_dependencies_exact && patch.applied_validation.lane_dependencies_exact,
           "commit family or resource dependencies diverged during materialization");
    expect(patch.shadow_rewrite.topology_valid, "commit family shadow plan contains a cycle");
    expect(markov::trace_graph::core::validate_active_dag(graph).ok(), "materialized commit family graph contains a cycle");
    expect(patch.source_attribution.records[3].owned_duration_nodes.empty(), "capacity gate incorrectly claimed release-control duration");
    expect(graph.node(8).duration == 150, "capacity gate changed release-control duration");
    expect(graph.node(6).duration == 0 && graph.node(10).duration == 0, "commit I/O source duration was not removed exactly once");

    std::map<std::string, size_t> synthetic_nodes;
    for (const auto & record : patch.journal.records) {
        if (record.action == markov::trace_graph::core::DagMutationAction::AddSyntheticNode && record.node_id)
            synthetic_nodes.emplace(record.effect_id, *record.node_id);
    }
    expect(graph.has_active_edge(synthetic_nodes.at("commit-d2h"), synthetic_nodes.at("commit-h2s"), DagEdgeKind::Mutation, "hicache_family_dependency"),
           "commit H2S does not wait for its D2H family predecessor");
    expect(graph.has_active_edge(synthetic_nodes.at("commit-h2s"), synthetic_nodes.at("commit-capacity"), DagEdgeKind::Mutation, "hicache_family_dependency"),
           "commit capacity gate does not wait for storage completion");
    expect(
        graph.has_active_edge(synthetic_nodes.at("earlier-prefetch-io"), synthetic_nodes.at("commit-h2s"), DagEdgeKind::Mutation, "scope:1/host_storage_lane"),
        "host-storage lane ordering did not coexist with commit family ordering");
}

HiCacheEffectDecision required_loadback_decision(std::optional<size_t> consumer_node_id = std::nullopt, std::string consumer_role = {}) {
    auto effect = decision("insert-loadback", HiCacheEffectType::Loadback, 0, "cache_lookup_input", "loadback-insert-request");
    effect.direction = HiCacheTransferDirection::HostToDevice;
    effect.effective_page_count = 1;
    effect.effective_byte_count = 100;
    effect.eligibility_boundary = { .kind = "cache_lookup", .epoch = 1, .timestamp_us = 10 };
    if (consumer_node_id) {
        effect.consumer_boundary = {
            .kind = "loadback_consumer",
            .epoch = 2,
            .timestamp_us = 40,
            .source_node_id = *consumer_node_id,
            .source_event_index = *consumer_node_id,
            .source_fact_role = std::move(consumer_role),
        };
    }
    return effect;
}

void check_source_absent_insertion_boundaries() {
    {
        std::vector<TraceEvent> events;
        events.push_back(fact_event(0,
                                    "workload_identity",
                                    "cache_lookup_input",
                                    10,
                                    1,
                                    {
                                        { "request_id", "loadback-insert-request" }
        }));
        events.push_back(fact_event(1,
                                    "source_actual",
                                    "loadback_decision_observed",
                                    20,
                                    1,
                                    {
                                        {            "request_id", "loadback-insert-request" },
                                        { "effective_token_count",                       "0" }
        }));
        events.push_back(fact_event(2,
                                    "source_actual",
                                    "request_admission_observed",
                                    40,
                                    1,
                                    {
                                        { "request_id", "loadback-insert-request" }
        }));
        DagGraph graph(std::move(events), 0);
        for (size_t event_index = 0; event_index < graph.events().size(); ++event_index) graph.add_node(event_index, true, "cpu");
        graph.add_edge(0, 1, DagEdgeKind::Sequential);
        graph.add_edge(1, 2, DagEdgeKind::Sequential);

        HiCacheDagPatchModule module(dependency_model_result(required_loadback_decision(2, "request_admission_observed")));
        module.apply(graph);
        const auto & patch = module.result();
        expect(patch.status == "applied", "source-absent loadback was not inserted at its canonical consumer");
        expect(patch.source_attribution.records[0].source_carrier_state == HiCacheSourceCarrierState::Absent,
               "explicit source loadback no-op was not classified as absent");
        expect(patch.source_attribution.records[0].consumer_anchor_method == "target_canonical_consumer",
               "canonical target consumer was not preferred for source-absent insertion");
    }
    {
        std::vector<TraceEvent> events;
        events.push_back(fact_event(0,
                                    "workload_identity",
                                    "cache_lookup_input",
                                    10,
                                    1,
                                    {
                                        { "request_id", "loadback-insert-request" }
        }));
        events.push_back(fact_event(1,
                                    "source_actual",
                                    "request_admission_observed",
                                    40,
                                    1,
                                    {
                                        { "request_id", "loadback-insert-request" }
        }));
        events.push_back(fact_event(2,
                                    "source_actual",
                                    "loadback_decision_observed",
                                    20,
                                    1,
                                    {
                                        {            "request_id", "loadback-insert-request" },
                                        { "effective_token_count",                       "0" }
        }));
        DagGraph graph(std::move(events), 0);
        for (size_t event_index = 0; event_index < graph.events().size(); ++event_index) graph.add_node(event_index, true, "cpu");
        graph.add_edge(0, 1, DagEdgeKind::Sequential);

        HiCacheDagPatchModule module(dependency_model_result(required_loadback_decision()));
        module.apply(graph);
        const auto & patch = module.result();
        expect(patch.status == "applied", "source-absent loadback rejected a unique original sequential successor");
        expect(patch.source_attribution.records[0].consumer_anchors == std::vector<size_t>{ 1 },
               "source-absent insertion selected the wrong sequential successor");
        expect(patch.source_attribution.records[0].consumer_anchor_method == "unique_original_sequential_successor",
               "sequential-successor fallback was not recorded explicitly");
    }
    {
        std::vector<TraceEvent> events;
        events.push_back(fact_event(0,
                                    "workload_identity",
                                    "cache_lookup_input",
                                    10,
                                    1,
                                    {
                                        { "request_id", "loadback-insert-request" }
        }));
        events.push_back(fact_event(1,
                                    "source_actual",
                                    "request_admission_observed",
                                    40,
                                    1,
                                    {
                                        { "request_id", "loadback-insert-request" }
        }));
        events.push_back(fact_event(2, "source_actual", "cache_extend_input", 50, 1));
        events.push_back(fact_event(3,
                                    "source_actual",
                                    "loadback_decision_observed",
                                    20,
                                    1,
                                    {
                                        {            "request_id", "loadback-insert-request" },
                                        { "effective_token_count",                       "0" }
        }));
        DagGraph graph(std::move(events), 0);
        for (size_t event_index = 0; event_index < graph.events().size(); ++event_index) graph.add_node(event_index, true, "cpu");
        graph.add_edge(0, 1, DagEdgeKind::Sequential);
        graph.add_edge(0, 2, DagEdgeKind::Sequential);

        HiCacheDagPatchModule module(dependency_model_result(required_loadback_decision()));
        module.apply(graph);
        const auto & patch = module.result();
        expect(patch.status == "blocked", "ambiguous sequential insertion boundary did not block the transaction");
        expect(patch.source_attribution.records[0].consumer_anchor_method == "ambiguous_sequential_successor",
               "ambiguous sequential fallback was not reported");
        expect(patch.shadow_rewrite.blocker_counts.contains("missing_insertion_consumer_anchor"),
               "ambiguous insertion boundary produced the wrong rewrite blocker");
        expect(patch.journal.records.empty(), "ambiguous insertion boundary mutated the graph");
    }
    {
        std::vector<TraceEvent> events;
        events.push_back(fact_event(0,
                                    "workload_identity",
                                    "cache_lookup_input",
                                    10,
                                    1,
                                    {
                                        { "request_id", "loadback-insert-request" }
        }));
        events.push_back(fact_event(1,
                                    "source_actual",
                                    "loadback_decision_observed",
                                    20,
                                    1,
                                    {
                                        {            "request_id", "loadback-insert-request" },
                                        { "effective_token_count",                       "0" }
        }));
        events.push_back(fact_event(2,
                                    "source_actual",
                                    "request_admission_observed",
                                    40,
                                    1,
                                    {
                                        { "request_id", "loadback-insert-request" }
        }));
        DagGraph graph(std::move(events), 0);
        for (size_t event_index = 0; event_index < graph.events().size(); ++event_index) graph.add_node(event_index, true, "cpu");
        graph.add_edge(0, 1, DagEdgeKind::Sequential);
        graph.add_edge(1, 2, DagEdgeKind::Sequential);

        HiCacheDagPatchModule module(dependency_model_result(required_loadback_decision(2, "cache_extend_input")));
        module.apply(graph);
        const auto & patch = module.result();
        expect(patch.status == "blocked", "invalid target consumer was silently replaced by a sequential fallback");
        expect(patch.source_attribution.records[0].source_carrier_state == HiCacheSourceCarrierState::Unobservable,
               "invalid target consumer did not invalidate source attribution");
        expect(patch.source_attribution.records[0].consumer_anchor_method == "invalid_target_canonical_consumer",
               "invalid target consumer provenance was not retained");
        expect(patch.journal.records.empty(), "invalid target consumer mutated the graph");
    }
}

void check_atomic_apply_gate() {
    {
        std::vector<TraceEvent> events;
        events.push_back(fact_event(0,
                                    "workload_identity",
                                    "prefetch_candidate_anchor",
                                    10,
                                    1,
                                    {
                                        { "request_id", "complete-no-op" }
        }));
        DagGraph graph(std::move(events), 0);
        graph.add_node(0, true, "cpu");
        auto effect = decision("complete-no-op", HiCacheEffectType::PrefetchVisibility, 0, "prefetch_candidate_anchor", "complete-no-op");
        effect.target_effect_state = HiCacheTargetEffectState::NotRequired;
        auto result = dependency_model_result(std::move(effect));
        result->effect_decisions.io_model_calibration_status = "contract_only";

        HiCacheDagPatchModule module(std::move(result));
        module.apply(graph);
        const auto & patch = module.result();
        expect(patch.status == "no_mutation_required", "complete no-op cell incorrectly required calibrated I/O parameters");
        expect(patch.applied_validation.status == "ready", "complete no-op cell failed post-apply validation");
        expect(patch.journal.records.empty(), "complete no-op cell produced graph mutations");
    }
    {
        auto graph = prefetch_patch_graph();
        HiCacheDagPatchModule module(patch_model_result("contract_only"));
        module.apply(graph);
        const auto & patch = module.result();
        expect(patch.status == "blocked", "contract-only model did not block production apply");
        expect(patch.applied_validation.status == "not_applied", "blocked transaction ran post-apply validation");
        expect(patch.apply_blockers.contains("io_model_not_calibrated"), "calibration blocker was not reported");
        expect(patch.journal.records.empty(), "contract-only model produced graph mutations");
        expect(graph.node_count() == 3 && graph.edge_count() == 2 && graph.node(1).duration == 25,
               "blocked transaction changed graph storage or source duration");
    }
    {
        auto graph = prefetch_patch_graph();
        HiCacheDagPatchModule module(patch_model_result("calibrated"));
        module.apply(graph);
        const auto & patch = module.result();
        expect(patch.status == "applied", "complete calibrated transaction was not applied");
        expect(patch.applied_validation.status == "ready", "complete calibrated transaction failed post-apply validation");
        expect(patch.apply_blockers.empty(), "ready transaction retained apply blockers");
        expect(!patch.journal.records.empty(), "applied transaction has an empty mutation journal");
        expect(graph.node_count() == 4 && graph.node(1).duration == 0, "applied transaction did not replace source-owned duration");
        expect(!graph.edge(0).active, "applied transaction left the original ingress bypass active");
        expect(graph.active_edge_count() == 3, "replacement transaction produced an unexpected active-edge shape");
        graph.set_node_duration(1, 25);
        const auto invalid = validate_hicache_applied_patch(graph, patch.shadow_rewrite, patch.io_resources, patch.journal, true);
        expect(invalid.status == "failed", "post-apply validation accepted a restored source duration");
        expect(invalid.blocker_counts.contains("source_duration_materialization_mismatch"), "post-apply validation reported the wrong source-duration blocker");
    }
    {
        auto graph = prefetch_patch_graph("different-request");
        HiCacheDagPatchModule module(patch_model_result("calibrated"));
        module.apply(graph);
        const auto & patch = module.result();
        expect(patch.status == "applied", "contract-proven source absence did not insert the target prefetch I/O");
        expect(patch.source_attribution.records[0].source_carrier_state == HiCacheSourceCarrierState::Absent,
               "different-request timing prevented exact-request absence classification");
        expect(graph.node_count() == 4 && graph.node(1).duration == 25, "source-absent insertion changed an unrelated request's timing duration");
    }
    {
        std::vector<TraceEvent> events;
        events.push_back(fact_event(0,
                                    "workload_identity",
                                    "prefetch_candidate_anchor",
                                    10,
                                    1,
                                    {
                                        { "request_id", "patch-request" }
        }));
        events.push_back(fact_event(1,
                                    "source_actual",
                                    "request_admission_observed",
                                    50,
                                    1,
                                    {
                                        { "request_id", "patch-request" }
        }));
        DagGraph graph(std::move(events), 0);
        graph.add_node(0, true, "cpu");
        graph.add_node(1, true, "cpu");
        graph.add_edge(0, 1, DagEdgeKind::Sequential);

        HiCacheDagPatchModule module(patch_model_result("calibrated", "patch-request", 1));
        module.apply(graph);
        const auto & patch = module.result();
        expect(patch.status == "blocked", "missing prefetch probe contract was guessed as source absence");
        expect(patch.apply_blockers.contains("source_attribution_not_ready"), "source-attribution blocker was not reported");
        expect(patch.journal.records.empty(), "unobservable source contract produced graph mutations");
    }
}

void check_partial_transfer_replacement() {
    auto graph = prefetch_patch_graph();
    auto result = patch_model_result("calibrated");
    auto & effect = result->effect_decisions.decisions.front();
    effect.target_effect_state = HiCacheTargetEffectState::Partial;
    effect.candidate_page_count = 2;

    HiCacheDagPatchModule module(std::move(result));
    module.apply(graph);
    const auto & patch = module.result();
    expect(patch.status == "applied", "effect-local partial transfer did not produce an atomic patch");
    expect(patch.shadow_rewrite.decisions.front().rewrite_kind == markov::trace_graph::modules::hicache::patch::HiCacheRewriteKind::PartialReplace,
           "partial transfer did not retain its explicit rewrite classification");
    expect(patch.shadow_rewrite.ownership_conflicts.empty(), "exclusive partial carrier produced an ownership conflict");
    expect(patch.applied_validation.status == "ready", "partial transfer failed post-apply validation");
    expect(graph.node(1).duration == 0, "partial replacement retained source-owned transfer duration");
}

} // namespace

int main() {
    check_exact_identity_chains();
    check_repeated_lookup_opportunity_window();
    check_manifest_contract_absence();
    check_atomic_apply_gate();
    check_partial_transfer_replacement();
    check_dependency_gate_ownership();
    check_prefetch_family_dependency();
    check_commit_family_and_resource_dependencies();
    check_source_absent_insertion_boundaries();
    return 0;
}
