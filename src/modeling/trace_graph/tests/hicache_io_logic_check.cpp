/** @file Explicit validation-build regressions; never part of profiling/prediction. */
#include "markov/trace_graph/modules/hicache/patch/io_resource_model.hpp"
#include "markov/trace_graph/modules/hicache/patch/applied_validator.hpp"
#include "markov/trace_graph/modules/hicache/patch/boundary_validator.hpp"
#include "markov/trace_graph/modules/hicache/patch/rewrite_transaction.hpp"
#include "markov/trace_graph/modules/hicache/service_model.hpp"
#include "markov/trace_graph/modules/hicache/runtime/device_allocator.hpp"
#include "markov/trace_graph/modules/hicache/runtime/preparation.hpp"
#include "markov/trace_graph/modules/hicache/model/state.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "../src/modules/hicache/patch/attribution_common.hpp"
#include "../src/modules/hicache/patch/rewrite_mutation.hpp"
#include "../src/modules/hicache/patch/io_operation_ledger_detail.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace markov::trace_graph;
using namespace markov::trace_graph::modules::hicache;

void check_hicache_phase_timing();
void check_hicache_preparation_costs();

namespace {

void require(bool value, const std::string & message) {
    if (!value) throw std::runtime_error(message);
}


void allocator_slice_follows_physical_operations() {
    runtime::DeviceAllocatorLedger allocator;
    allocator.configure(48, false);
    require(allocator.allocate(17) == 17 && allocator.free_index_offset == 17, "loadback consumes a free-index prefix");
    allocator.configure(48, false);
    require(allocator.free_index_offset == 17, "reusing configuration must not imply a fresh tensor");
    allocator.allocate(1);
    require(allocator.free_index_offset == 18, "extend advances the same tensor slice");
    allocator.release(1);
    require(allocator.free_index_offset == 0, "immediate release concatenates a fresh tensor");
    allocator.allocate(3);
    allocator.release(0);
    allocator.merge_release_pages();
    require(allocator.free_index_offset == 3, "empty release and merge do not rebuild the free tensor");
    allocator.reconcile_occupied_pages(20, 0);
    require(allocator.available_pages() == 28 && allocator.free_index_offset == 3, "consistent count audit preserves slice identity");
    allocator.reconcile_occupied_pages(19, 0);
    require(!allocator.free_index_offset, "unexplained count correction cannot imply physical alignment");
    allocator.allocate(1);
    require(!allocator.free_index_offset, "consumption cannot repair an unknown slice origin");
    allocator.release(1);
    require(allocator.free_index_offset == 0, "a later real concatenation restores a known origin");

    allocator.configure(48, true);
    allocator.allocate(17);
    allocator.release(4);
    require(allocator.free_index_offset == 17 && allocator.release_pages == 4, "deferred release does not rebuild free_pages");
    allocator.merge_release_pages();
    require(allocator.free_index_offset == 0 && allocator.free_pages == 35, "nonempty merge creates a fresh tensor");
    allocator.allocate(1);
    allocator.merge_release_pages();
    require(allocator.free_index_offset == 1, "empty sorted merge preserves the offset");
    allocator.allocate(100);
    require(!allocator.free_index_offset, "partial count consumption does not prove a successful physical allocation");
}

void source_prefetch_wait_is_removed_without_a_target_join() {
    core::DagGraph graph;
    const auto before = graph.add_synthetic_node({.name = "source polling", .duration = 1});
    const auto after = graph.add_synthetic_node({.name = "consumer", .duration = 1});
    graph.add_edge(before, after, core::DagEdgeKind::Sequential);
    graph.mutable_node(before).cpu_gap_after = graph.mutable_node(before).original_cpu_gap_after = 100;
    patch::HiCacheRewriteDecision decision;
    decision.effect_id = "prefetch";
    decision.effect_type = model::HiCacheEffectType::PrefetchIo;
    decision.rewrite_kind = patch::HiCacheRewriteKind::RemoveOwnedCost;
    decision.shadow_plan_ready = true;
    decision.completion_join_contract_ready = true;
    decision.source_completion_wait_blocking = true;
    decision.completion_wait_slices = {{.owner_node_id = before, .successor_node_id = after,
        .gap_start_us = 1, .gap_end_us = 101, .owned_start_us = 21, .owned_end_us = 81}};
    const auto plan = patch::rewrite_transaction_detail::build_plan(graph, {decision}, {});
    require(plan.set_cpu_gaps.size() == 1 && plan.set_cpu_gaps.front().duration == 40,
            "target without a completion join still removes proven source polling wait, retaining unrelated time");
    decision.source_completion_wait_blocking = false;
    require(patch::rewrite_transaction_detail::build_plan(graph, {decision}, {}).set_cpu_gaps.empty(),
            "no proven source wait means no invented removal");
    decision.source_completion_wait_blocking = true;
    decision.rewrite_kind = patch::HiCacheRewriteKind::NoOp;
    require(patch::rewrite_transaction_detail::build_plan(graph, {decision}, {}).empty(), "self no-op preserves source waiting");
}

void false_progress_owns_its_enclosing_call() {
    const auto event = [](std::string name, uint64_t ts, uint64_t dur) {
        core::TraceEvent e;
        e.name = std::move(name); e.index = ts; e.ts = ts; e.dur = dur;
        e.pid = e.tid = "1"; e.cat = "cpu_op";
        return e;
    };
    auto graph = core::DagBuilder(1).build({event("before", 0, 5),
        event("hicache.control.prefetch_progress", 10, 30), event("false child", 22, 1),
        event("unrelated scheduler work", 50, 5),
        event("hicache.control.prefetch_progress", 65, 25), event("terminal child", 73, 1),
        event("after", 100, 5)}, 0);
    const auto check = [&](size_t id, uint64_t ts, bool ready) {
        auto e = event("progress fact", ts, 5);
        e.index = id; e.source_channel = core::TraceSourceChannel::PythonProbe;
        e.set_arg("fact", R"({"class":"source_actual","role":"prefetch_progress_observed","consumers":["hicache_dag_patch"]})");
        e.set_arg("phase", "end"); e.set_arg("request_id", "request");
        e.set_arg("progress_ready", ready ? "true" : "false");
        return e;
    };
    graph.set_hicache_fact_events({check(0,20,false), check(1,70,true)});
    const patch::HiCacheSourceDagIndex index(graph);
    patch::HiCacheIoOperationRecord record;
    record.request_id = "request"; record.pid = "1"; record.source_end_us = 60;
    patch::io_operation_ledger_detail::build_prefetch_completion_wait_contract(index,record);
    require(record.completion_join_contract_ready, "fixture proves false-to-completion-to-true order");
    uint64_t removed = 0;
    for (const auto id : record.completion_wait_owned_node_ids) {
        const auto& e = graph.event_for_node(id);
        require(e.ts >= 10 && e.ts + e.dur <= 40, "only the false call is owned, not terminal or intervening scheduler work");
        removed += graph.node(id).duration;
    }
    require(removed == 30, "the false call owns its wrapper self pieces even when the fact cuts through them");
    require(record.progress_check_cpu_samples_us == std::vector<uint64_t>{1},
            "calibration keeps only explicit function work, not wrapper/probe overhead");
    auto bare = core::DagBuilder(1).build({event("before",0,5), event("false child",22,1),
        event("unrelated scheduler work",50,5), event("terminal child",73,1), event("after",100,5)},0);
    bare.set_hicache_fact_events(graph.hicache_fact_events());
    const patch::HiCacheSourceDagIndex bare_index(bare);
    patch::HiCacheIoOperationRecord bare_record;
    bare_record.request_id = "request"; bare_record.pid = "1"; bare_record.source_end_us = 60;
    patch::io_operation_ledger_detail::build_prefetch_completion_wait_contract(bare_index,bare_record);
    require(bare_record.completion_join_contract_ready && bare_record.completion_wait_owned_node_ids.size() == 1
            && bare.event_for_node(bare_record.completion_wait_owned_node_ids.front()).name == "false child",
            "without an enclosing marker, retain the narrower proven ownership");
}

void allocator_preparation_uses_source_coverage_and_target_history() {
    core::TraceEvent before;
    before.name = "before allocation"; before.pid = before.tid = "1"; before.dur = 10;
    auto after = before; after.name = "Enqueue@alloc_extend_kernel"; after.ts = 100;
    auto observation = before;
    observation.name = "runtime.triton.prepare"; observation.cat = "runtime_diagnostic";
    observation.source_channel = core::TraceSourceChannel::PythonProbe;
    observation.ts = 20; observation.dur = 40;
    observation.set_arg("kernel", "alloc_extend_kernel");
    observation.set_arg("status", "returned");
    observation.set_arg("constants", R"({"page_size":64,"bs_upper":1,"max_num_extend_tokens":64,"BLOCK_SIZE":2048})");
    observation.set_arg("signature", R"({"pre_lens_ptr":"*i64","seq_lens_ptr":"*i64","last_loc_ptr":"*i64","free_page_ptr":"*i64","out_indices":"*i64"})");
    observation.set_arg("argument_properties", R"({"tt.divisibility":[0,1,2,3,4]})");
    auto load = observation; load.name = "runtime.triton.load"; load.ts = 55; load.dur = 20;
    load.set_arg("kernel", "alloc_extend_kernel_aiv");
    auto graph = core::DagBuilder(1).build({before, after, observation, load}, 0);
    auto fact = before; fact.name = "cache_extend_input"; fact.ts = 10; fact.dur = 0;
    graph.set_hicache_fact_events({fact});
    model::HiCacheAllocatorWorkItem prelude{.pid = "1", .formal = false, .page_size = 64,
        .batch_size = 1, .extend_tokens = 64, .allocated_pages = 1, .free_index_offset = 0};
    auto formal = prelude; formal.formal = true;
    auto plan = runtime::plan_allocator_preparations(graph, {prelude, formal});
    require(plan.status == "ready" && plan.calls[1].status == "already_prepared", "target prelude prepares the formal variant");
    require(plan.observed_formal_calls == 1 && plan.removed_coverage_us == 55
            && plan.mutation.set_cpu_gaps.size() == 1 && plan.mutation.set_cpu_gaps[0].duration == 35,
            "prepare/load overlap is counted once; uncovered CPU time remains");
    require(graph.node(0).cpu_gap_after == 90, "planning must not edit the source graph");
    const auto self = runtime::plan_allocator_preparations(graph, {formal});
    require(self.status == "ready" && self.calls[0].status == "required" && self.mutation.empty(),
            "same required variant retains its measured preparation cost");
    auto before2 = before, after2 = after, observation2 = observation, load2 = load, fact2 = fact;
    for (auto* value : {&before2, &after2, &observation2, &load2, &fact2}) value->pid = value->tid = "2";
    auto other_rank = core::DagBuilder(1).build({before2, after2, observation2, load2}, 1);
    other_rank.set_hicache_fact_events({fact2});
    auto merged = core::DagGraph::merge({graph, other_rank});
    auto prelude2 = prelude, formal2 = formal;
    prelude2.pid = formal2.pid = "2"; formal2.source_fact_id = 1;
    const auto two_ranks = runtime::plan_allocator_preparations(merged, {prelude, prelude2, formal, formal2});
    require(merged.runtime_observations().size() == 4 && two_ranks.status == "ready" && two_ranks.mutation.set_cpu_gaps.size() == 2
            && two_ranks.removed_coverage_us == 110, "rank-local preparation identities and coverage survive graph merge");
    formal.free_index_offset = 1;
    const auto alignment = runtime::plan_allocator_preparations(graph, {prelude, formal});
    require(alignment.status == "partial" && alignment.calls[1].status == "required" && alignment.mutation.empty(),
            "pointer alignment creates a distinct variant; a different source cost is not silently reused");
    formal.free_index_offset.reset();
    require(runtime::plan_allocator_preparations(graph, {prelude, formal}).status == "partial", "unknown physical slice is not a cache hit");
    formal.allocated_pages = 200;
    require(runtime::plan_allocator_preparations(graph, {formal}).removed_coverage_us == 55, "naive path requires no Triton preparation");
    formal = prelude; formal.formal = true;
    prelude.free_index_offset.reset();
    require(runtime::plan_allocator_preparations(graph, {prelude, formal}).calls[1].status == "possibly_required",
            "unknown prelude propagates uncertainty to first use of a variant");
    prelude = formal; prelude.formal = false;
    graph.mutable_node(0).cpu_gap_after = 80;
    require(runtime::plan_allocator_preparations(graph, {prelude, formal}).mutation.empty(), "never overwrite a previously transformed gap");
    graph.mutable_node(0).cpu_gap_after = 90;
    observation.ts = 5;
    graph.set_runtime_observations({observation, load});
    require(runtime::plan_allocator_preparations(graph, {formal}).status == "partial", "load alone cannot prove preparation cost coverage");
    graph.set_runtime_observations({});
    require(runtime::plan_allocator_preparations(graph, {prelude, formal}).status == "unavailable", "missing probe is not a zero-cost prediction");
}

void prefill_batch_records_one_preallocation_slice() {
    frontend::HiCacheConfig config;
    config.page_size = 64;
    config.l1_capacity_pages = 48;
    config.l2_capacity_pages = 64;
    model::HiCacheState state(config);
    for (uint32_t batch = 0; batch < 2; ++batch) {
        HiCacheFact fact;
        fact.pid = fact.tid = "worker";
        fact.cache_scope = "cache";
        fact.fact_class = "workload_identity";
        fact.role = "cache_extend_input";
        fact.consumers = {"hicache_state_model"};
        fact.is_start = true;
        fact.ts = batch + 1;
        for (uint32_t index = 0; index < 2; ++index) {
            HiCacheBatchPathEntry entry;
            entry.request_id = "request-" + std::to_string(batch * 2 + index);
            entry.position = index;
            entry.token_count = 64;
            entry.full_path_span = {.path_id = entry.request_id, .begin = 0, .end = 64, .token_count = 64, .valid = true};
            for (uint32_t token = 0; token < 64; ++token) entry.full_path_tokens.push_back({{batch * 128 + index * 64 + token}});
            fact.batch_paths.push_back(std::move(entry));
        }
        state.apply_fact(fact, HiCacheFactRole::CacheExtendInput, batch > 0);
    }
    const auto & work = state.allocator_work_items();
    require(state.prefill_work_items().size() == 2 && work.size() == 2, "prelude allocator calls are retained without adding formal compute requests");
    for (size_t index = 0; index < work.size(); ++index) {
        require(work[index].batch_size == 2 && work[index].request_ids.size() == 2 && work[index].free_index_offset == index * 2,
                "batch members share the slice before the joint allocation, not per-request offsets");
        require(work[index].formal == (index > 0), "formal boundary must not reset preparation history");
    }
}

void extend_respects_lookup_input_limit() {
    const auto check = [](uint64_t page_size, uint64_t limit, bool unfinished) {
        frontend::HiCacheConfig config;
        config.write_policy = "write_back";
        config.page_size = page_size;
        config.l1_capacity_pages = 48;
        config.l2_capacity_pages = 64;
        model::HiCacheState state(config);
        const auto fact = [](std::string role, std::string request, uint64_t count) {
            HiCacheFact f;
            f.pid = f.tid = "worker";
            f.cache_scope = "cache";
            f.fact_class = "workload_identity";
            f.consumers = {"hicache_state_model"};
            f.role = std::move(role);
            f.request_id = std::move(request);
            f.is_end = true;
            f.token_count = count;
            f.full_path_span = {.path_id = "path", .begin = 0, .end = count, .token_count = count, .valid = true};
            for (uint32_t token = 0; token < count; ++token) f.full_path_tokens.push_back({{token}});
            return f;
        };
        auto seed = fact("cache_lifecycle_commit", "seed", 128);
        seed.lifecycle_kind = unfinished ? "unfinished" : "finished";
        state.apply_fact(seed, HiCacheFactRole::CacheLifecycleCommit, false);
        const auto request = unfinished ? "seed" : "repeat";
        const uint64_t input_tokens = unfinished ? 192 : 128;
        const uint64_t prefix_tokens = unfinished ? 128 : limit / page_size * page_size;
        auto lookup = fact("cache_lookup_input", request, limit);
        state.apply_fact(lookup, HiCacheFactRole::CacheLookupInput, false);
        auto extend = fact("cache_extend_input", request, input_tokens);
        extend.is_end = false;
        extend.is_start = true;
        HiCacheBatchPathEntry entry;
        entry.request_id = request;
        entry.token_count = input_tokens;
        entry.full_path_span = extend.full_path_span;
        entry.full_path_tokens = extend.full_path_tokens;
        extend.batch_paths = {entry};
        state.apply_fact(extend, HiCacheFactRole::CacheExtendInput, true);
        const auto & work = state.prefill_work_items().back();
        require(work.prompt_token_count == input_tokens && work.reusable_prefix_token_count == prefix_tokens,
                "extend must not rediscover cached pages beyond the request's lookup input");
        require(work.prefill_token_count == input_tokens - prefix_tokens,
                "full cache residency does not remove required final-token or logprob computation");
    };
    for (const uint64_t page_size : {32, 64})
        for (const uint64_t limit : {127, 63, 0})
            for (const bool unfinished : {false, true}) check(page_size, limit, unfinished);
}

void foreground_projection_does_not_remove_queue_wait_twice() {
    const auto event = [](std::string name, std::string tid, uint64_t ts, uint64_t dur, std::string cat = "cpu_op") {
        core::TraceEvent e;
        e.name = std::move(name); e.pid = "1"; e.tid = std::move(tid);
        e.ts = ts; e.dur = dur; e.cat = std::move(cat);
        return e;
    };
    auto submit = event("submit", "producer", 0, 100, "enqueue");
    auto next = event("next task", "worker", 106, 5, "dequeue");
    submit.set_arg("correlation_id", "task"); next.set_arg("correlation_id", "task");
    auto graph = core::DagBuilder(1).build({
        submit, next, event("previous task", "worker", 0, 10),
        event("foreground begin", "foreground", 0, 10), event("foreground end", "foreground", 106, 1),
        event("unknown begin", "unknown", 0, 10), event("unknown end", "unknown", 106, 1)}, 0);
    const auto id = [&](std::string_view name) {
        return std::ranges::find_if(graph.nodes(), [&](const auto& n) { return graph.event_for_node(n.id).name == name; })->id;
    };
    const std::vector<patch::HiCacheCpuGapSlice> foreground{{id("foreground begin"), id("foreground end"), 0, 10, 106, 20, 80}};
    const patch::HiCacheSourceDagIndex index(graph);
    const auto projected = index.project_foreground_gap_across_logical_input_lanes(foreground);
    require(projected.size() == 1 && projected.front().owner_node_id == id("unknown begin")
                && projected.front().owned_duration_us() == 60,
            "only the still-fixed unknown gap can be projected; task-arrival wait already follows its dependency");
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

void resource_validation_distinguishes_preservation_from_rewrite() {
    core::DagGraph graph;
    const auto first = graph.add_synthetic_node({.name = "first I/O", .duration = 10});
    const auto second = graph.add_synthetic_node({.name = "second I/O", .duration = 20});
    const auto phase = graph.add_synthetic_node({.name = "compute", .duration = 30});
    const auto edge = graph.add_edge(first, second, core::DagEdgeKind::Sequential);
    patch::HiCacheShadowRewriteTransaction shadow;
    shadow.topology_valid = true;
    for (const auto id : {"first", "second"}) {
        patch::HiCacheRewriteDecision decision;
        decision.effect_id = id;
        decision.rewrite_kind = patch::HiCacheRewriteKind::NoOp;
        decision.shadow_plan_ready = true;
        shadow.decisions.push_back(decision);
    }
    patch::HiCacheIoResourcePlan resources;
    resources.lane_dependencies = {{"io_lane", "first", "second"}};
    core::DagMutationPlan plan;
    plan.component = "hicache";
    plan.set_node_durations = {{.node_id = phase, .duration = 25, .effect_id = "prefill"}};
    const auto validate = [&](core::DagGraph candidate, const auto & transaction, const auto & lanes, const auto & changes) {
        const auto mutation = core::apply_dag_mutation_plan(candidate, changes);
        return patch::validate_hicache_applied_patch(candidate, transaction, lanes, changes, mutation, true);
    };
    require(validate(graph, shadow, resources, plan).status == "ready",
            "an empty I/O transaction preserves source resources while an independent phase may change");
    auto with_phase_edge = plan;
    with_phase_edge.add_edges.push_back({.src=core::DagNodeRef::existing(phase), .dst=core::DagNodeRef::existing(second),
        .kind=core::DagEdgeKind::Mutation, .effect_id="prefill_boundary"});
    auto candidate = graph;
    auto mutation = core::apply_dag_mutation_plan(candidate, with_phase_edge);
    require(patch::validate_hicache_applied_patch(candidate,shadow,resources,with_phase_edge,mutation,true).plan_journal_exact,
            "an indexed phase edge must match its actual materialized edge");
    const auto added = std::ranges::find_if(mutation.journal.records, [](const auto& record) {
        return record.action == core::DagMutationAction::AddEdge;
    });
    require(added != mutation.journal.records.end(), "fixture must contain an added-edge journal record");
    const auto duplicate = *added;
    mutation.journal.records.push_back(duplicate);
    require(!patch::validate_hicache_applied_patch(candidate,shadow,resources,with_phase_edge,mutation,true).plan_journal_exact,
            "indexing must still reject duplicate edge journal records");
    auto unknown = resources;
    unknown.lane_dependencies.front().successor_effect_id = "missing";
    require(!validate(graph, shadow, unknown, plan).lane_dependencies_exact, "preservation cannot hide an unknown effect");

    auto changed_noop = plan;
    changed_noop.set_node_durations.push_back({.node_id = first, .duration = 1, .effect_id = "first"});
    require(validate(graph, shadow, resources, changed_noop).status == "failed", "NoOp must reject actual effect mutations");
    auto extra_lane = plan;
    extra_lane.add_edges.push_back({.src = core::DagNodeRef::existing(first), .dst = core::DagNodeRef::existing(second),
                                   .kind = core::DagEdgeKind::Mutation, .effect_id = "io_lane"});
    require(!validate(graph, shadow, resources, extra_lane).lane_dependencies_exact, "preservation cannot silently add target resource edges");

    auto rewritten = shadow;
    for (size_t index = 0; index < rewritten.decisions.size(); ++index) {
        auto & decision = rewritten.decisions[index];
        decision.rewrite_kind = patch::HiCacheRewriteKind::ReplaceWithIo;
        decision.source_readiness_topology_reused = true;
        decision.owned_duration_nodes = {index == 0 ? first : second};
    }
    // These minimal decisions test resource validation only, not the other effect contracts.
    require(validate(graph, rewritten, resources, plan).lane_dependencies_exact, "a retained source edge can satisfy an actual rewrite");
    auto missing_edge = plan;
    missing_edge.disable_edges = {edge};
    require(!validate(graph, rewritten, resources, missing_edge).lane_dependencies_exact, "a removed required resource edge is still rejected");
    rewritten.decisions.front() = shadow.decisions.front();
    require(!validate(graph, rewritten, resources, plan).lane_dependencies_exact, "mixed preservation requires explicit source endpoints");
    auto nonempty_shadow = shadow;
    nonempty_shadow.plan = plan;
    require(!validate(graph, nonempty_shadow, resources, plan).lane_dependencies_exact, "a nonempty I/O transaction cannot claim full preservation");
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
    check_hicache_phase_timing();
    check_hicache_preparation_costs();
    source_prefetch_wait_is_removed_without_a_target_join();
    false_progress_owns_its_enclosing_call();
    allocator_slice_follows_physical_operations();
    allocator_preparation_uses_source_coverage_and_target_history();
    prefill_batch_records_one_preallocation_slice();
    extend_respects_lookup_input_limit();
    resource_validation_distinguishes_preservation_from_rewrite();
    foreground_projection_does_not_remove_queue_wait_twice();
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
