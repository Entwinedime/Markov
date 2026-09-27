#pragma once
#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/modules/hicache/model/state.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hicache_timing_fixture {
using namespace markov::trace_graph;
using namespace markov::trace_graph::modules::hicache;

inline void require(bool condition, const std::string & message) {
    if (!condition) throw std::runtime_error(message);
}

// Baseline two-rank synchronous MIN; scenarios override differing fields,
// including ranks whose native sequence counters differ.
inline void set_collective_args(core::TraceEvent & event, int rank, int index, const char * group = "4") {
    event.set_arg("group", group);
    event.set_arg("members", "[0,1]");
    event.set_arg("rank", std::to_string(rank));
    event.set_arg("collective_index", std::to_string(index));
    event.set_arg("sequence_before", std::to_string(index));
    event.set_arg("sequence_after", std::to_string(index + 1));

    event.set_arg("operation", "all_reduce");
    event.set_arg("numel", "1");
    event.set_arg("dtype", "torch.int32");
    event.set_arg("reduce_op", "MIN");
    event.set_arg("async_op", "false");
    event.set_arg("status", "returned");
}

template <typename Error, typename Operation> inline void require_throws(Operation && operation, const char * message) {
    try {
        operation();
    }
    catch (const Error &) {
        return;
    }

    throw std::runtime_error(message);
}

inline simulation::SimulationResult require_static_replay(core::DagGraph & graph, const char * message) {
    std::vector<std::pair<uint64_t, uint64_t>> timings;
    for (const auto & node : graph.nodes()) timings.emplace_back(node.simulation_start, node.completion_time);

    const auto result = simulation::run_topological_simulation(graph);
    for (const auto & node : graph.nodes()) {
        if (timings.at(node.id) != std::pair{ node.simulation_start, node.completion_time }) throw std::runtime_error(message);
    }
    return result;
}

inline HiCacheFact request_fact(std::string role, std::string request, uint64_t ts, uint32_t first_token) {
    HiCacheFact fact;
    fact.pid = fact.tid = "worker";
    fact.cache_scope = "cache";
    fact.fact_class = "workload_identity";
    fact.consumers = { "hicache_state_model" };
    fact.role = std::move(role);
    fact.request_id = std::move(request);
    fact.source_node_id = fact.source_event_index = ts;
    fact.source_ts = fact.ts = ts;
    fact.is_end = true;
    fact.chunked = false;
    fact.token_count = 32;
    fact.full_path_span = { .path_id = std::to_string(first_token), .begin = 0, .end = 32, .token_count = 32, .valid = true };
    for (uint32_t token = first_token; token < first_token + 32; ++token) fact.full_path_tokens.push_back({ { token } });
    return fact;
}

inline frontend::HiCacheConfig timing_config() {
    frontend::HiCacheConfig config;
    config.enabled = true;
    config.page_size = config.kv_bytes_per_page = 16;
    config.l1_capacity_pages = 4;
    config.l2_capacity_pages = 8;
    config.write_policy = "write_through";
    config.write_through_threshold = 1;
    config.prefetch_policy = "wait_complete";
    config.prefetch_threshold_pages = 1;
    config.io_cost.storage_batch_pages = 1;
    // Synthetic 10 us/page services exercise causality, not calibration accuracy.
    for (const auto & [kind, direction] : std::vector<std::pair<std::string, std::string>>{
             {              "prefetch", "storage_to_host" },
             {                  "load",  "host_to_device" },
             {  "write_device_to_host",  "device_to_host" },
             { "write_host_to_storage", "host_to_storage" }
    }) {
        auto & service = config.io_cost.service_models[kind];
        service.direction = direction;
        if (kind == "prefetch") service.stages = frontend::HiCachePrefetchStagesConfig{ .before_copy_us_per_byte = 0.625 };
        service.page_bandwidth_points = {
            { 16, 1'600'000, 0 }
        };
        service.new_operation_points = {
            { 16, 0, 1'600'000 }
        };
        service.existing_key_bandwidth_points = {
            { 16, 1, 1'600'000 }
        };
        config.io_cost.control_models[kind].fixed_us_per_operation = 1;
    }
    return config;
}

inline void seed_storage(model::HiCacheState & state, const std::string & scope = "cache", bool short_first_prefix = false) {
    for (uint32_t i = 0; i < 12; ++i) {
        auto seed = request_fact("cache_lifecycle_commit", "seed" + std::to_string(i), (i + 1) * 10'000, i * 32);
        seed.cache_scope = scope;
        if (i == 0 && short_first_prefix) {
            seed.token_count = seed.full_path_span.end = seed.full_path_span.token_count = 16;
            seed.full_path_tokens.resize(16);
        }
        seed.lifecycle_kind = "finished";
        state.apply_fact(seed, HiCacheFactRole::CacheLifecycleCommit, false);
        state.finalize();
    }
}

// These fixtures explicitly use zero query/MIN latency and identical local
// and global hits. Asymmetric-rank tests sample and complete separately.
inline void complete_query(model::HiCacheState & state, const HiCacheFact & fact) {
    const auto local = state.query_prefetch_storage(fact);
    state.complete_prefetch_query(fact, local);
}

// Isolated state fixtures execute the synthetic 10-us/page DMA explicitly.
// Production must instead call back at its DAG device-completion node.
inline void complete_test_writes(model::HiCacheState & state, const HiCacheFact & boundary) {
    for (const auto & write : state.pending_device_writes(boundary)) {
        if (write.schedule.ready_ts > boundary.ts) continue;
        auto completion = boundary;
        completion.ts = write.schedule.ready_ts;
        state.complete_device_write(completion, write.header.operation_id);
    }
}


} // namespace hicache_timing_fixture
