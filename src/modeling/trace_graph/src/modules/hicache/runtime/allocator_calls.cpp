#include "markov/trace_graph/modules/hicache/runtime/allocator_calls.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/allocator_program.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/source_calls.hpp"
#include "modules/hicache/runtime/source_preparation.hpp"

#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/host_removal.hpp"
#include "markov/trace_graph/modules/hicache/runtime/tensor_program.hpp"
#include "markov/trace_graph/source_dag/call_regions.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheAllocatorCalls::bind(core::DagGraph & graph, const HiCacheSourcePreparation & prepared, std::span<const model::HiCacheReplayFact> facts,
                                 const structure::hicache::SchedulerComputeResources & resources, uint64_t begin_us, uint64_t end_us) {
    const auto & original = prepared.index.graph();
    const auto & preparation = prepared.allocator;
    using Lane = std::pair<std::string, std::string>;
    std::map<Lane, std::vector<const HiCacheFact *>> outer_calls;
    std::map<size_t, const HiCacheFact *> extend_facts;
    size_t expected = 0;
    for (const auto & item : facts) {
        if (item.fact.source_ts < begin_us || item.fact.source_ts >= end_us) continue;
        if (item.role == HiCacheFactRole::CacheExtendInput) {
            extend_facts.emplace(item.fact.source_node_id, &item.fact);
            ++expected;
            continue;
        }
        if (item.role != HiCacheFactRole::CacheDecodeAllocation) continue;
        outer_calls[{ item.fact.pid, item.fact.tid }].push_back(&item.fact);
        ++expected;
    }
    for (auto & [lane, calls] : outer_calls) std::ranges::sort(calls, {}, [](const auto * fact) { return fact->source_ts; });

    const auto & original_index = prepared.index;
    const auto & original_queues = prepared.queues;
    const auto & regions = prepared.allocator_regions;
    struct Selection {
        const HiCacheFact * fact;
        const adapters::sglang::AllocatorRegion * region;
        const adapters::sglang::AllocatorPreparationSourceCall * preparation;
    };
    std::map<size_t, const adapters::sglang::AllocatorPreparationSourceCall *> extend_observations;
    for (const auto & call : preparation.calls) {
        if (!extend_facts.contains(call.source_fact_id)) continue;
        if (!call.allocator_observation || !call.issues.empty() || !call.allocator_work || !call.prefix_dtype || !call.page_count_supplied)
            throw std::runtime_error("Extend allocator lacks complete inner-call work metadata");
        extend_observations.emplace(*call.allocator_observation, &call);
    }
    std::vector<Selection> selected;
    std::vector<core::TraceEvent> points;
    std::set<size_t> paired;
    for (const auto & region : regions) {
        const auto & event = original.runtime_observations().at(region.work.observation);
        const auto finish = core::checked_add_u64(event.ts, event.dur, "Allocator end overflow");
        if (!is_admitted_execution_call(event, begin_us, end_us)) continue;

        const HiCacheFact * fact;
        const adapters::sglang::AllocatorPreparationSourceCall * observed_preparation = nullptr;
        if (region.phase == model::HiCacheAllocatorPhase::Extend) {
            const auto observed = extend_observations.find(region.work.observation);
            if (observed == extend_observations.end()) throw std::runtime_error("Extend allocator has no uniquely observed workload boundary");
            observed_preparation = observed->second;
            fact = extend_facts.at(observed_preparation->source_fact_id);
        }
        else {
            const auto lane = outer_calls.find({ event.pid, event.tid });
            if (lane == outer_calls.end()) throw std::runtime_error("Decode allocator has no workload boundary");
            auto outer = std::ranges::upper_bound(lane->second, event.ts, {}, [](const auto * item) { return item->source_ts; });
            if (outer == lane->second.begin()) throw std::runtime_error("Decode allocator precedes its workload boundary");
            fact = *--outer;
            if (finish > core::checked_add_u64(fact->source_ts, fact->dur, "Decode outer call end overflow"))
                throw std::runtime_error("Decode allocator exceeds its workload boundary");
        }

        if (!paired.insert(fact->source_node_id).second || region.allocated != true
            || (region.work.caller.status != "ready" && region.work.caller.status != "zero_duration"))
            throw std::runtime_error("Allocator lacks a unique complete successful source region");

        selected.push_back({ fact, &region, observed_preparation });
        for (const auto at : { event.ts, finish }) {
            core::TraceEvent point;
            point.name = "allocator.replacement";
            point.pid = event.pid;
            point.tid = event.tid;
            point.ts = at;
            points.push_back(std::move(point));
        }
    }
    if (paired.size() != expected) throw std::runtime_error("Workload lacks complete inner allocator observations");
    if (selected.empty()) return;

    const auto boundaries = bind_hicache_control_points(graph, points);
    const adapters::sglang::HiCacheSourceIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    std::vector<HiCacheHostTemplate> hosts;
    hosts.reserve(selected.size());
    std::vector<HiCacheHostRegion> removals;
    std::map<std::pair<size_t, size_t>, std::optional<HostDeviceResources>> observed_resources;
    for (size_t i = 0; i < selected.size(); ++i) {
        const auto & region = *selected[i].region;
        const auto & event = original.runtime_observations().at(region.work.observation);
        if (!boundaries[2 * i] || !boundaries[2 * i + 1]) throw std::runtime_error("Allocator requires exact entry and exit boundaries");
        const auto compute = resources.find({ event.pid, event.tid });
        const auto deployment = adapters::sglang::observed_allocator_inputs(event);
        if (compute == resources.end() || !deployment) throw std::runtime_error("Allocator lacks compute resource or deployment metadata");
        const auto resource_key = std::pair{ graph.node(*boundaries[2 * i]).lane_id, graph.node(compute->second).lane_id };
        const auto [resource, added] = observed_resources.try_emplace(resource_key);
        if (added) resource->second = observe_host_device_resources(source, queues, *boundaries[2 * i], compute->second);
        const auto & execution = resource->second;
        if (!execution) throw std::runtime_error("Allocator lacks a proven submission worker");

        const auto entry = *boundaries[2 * i], exit = *boundaries[2 * i + 1];
        hosts.push_back(observe_host_template(source, event.pid, event.tid, event.ts, points[2 * i + 1].ts));
        removals.push_back({ entry, exit, &hosts.back() });
        const auto * observed_preparation = selected[i].preparation;
        auto source_work = observed_preparation ? observed_preparation->allocator_work : adapters::sglang::observed_allocator_work(event);
        if (source_work && !observed_preparation) {
            const auto & fact = *selected[i].fact;
            for (size_t request = 0; request < fact.batch_paths.size(); ++request) {
                const auto length =
                    core::checked_add_u64(fact.batch_paths[request].token_count, fact.decode_iterations[request], "Source Decode allocation length overflow");
                source_work->requests.push_back({ fact.batch_paths[request].request_id,
                                                  core::checked_add_u64(length, 1, "Source Decode next length overflow"),
                                                  length,
                                                  fact.decode_iterations[request] });
            }
        }
        auto retained = prepare_host_expansion(source, queues, hosts.back(), "retained allocator: ");
        std::map<size_t, std::vector<size_t>> original_nodes;
        for (const auto & [original_node, copied_node] : retained.source_nodes) original_nodes[copied_node].push_back(original_node);
        for (const auto & [copied_node, members] : original_nodes)
            retained.nodes.at(copied_node).work.attrs["retained_allocator_source_nodes"] = nlohmann::json(members).dump();
        Call call{ exit, event.pid, event.tid, event.ts, *execution, *deployment, std::move(source_work), std::move(retained), {}, std::nullopt };
        if (observed_preparation) {
            call.prefix_dtype = observed_preparation->prefix_dtype;
            call.page_count_supplied = observed_preparation->page_count_supplied;
            call.source_preparation = adapters::sglang::AllocatorPreparation{ observed_preparation->required ? "required" : "not_required",
                                                                              observed_preparation->specialization,
                                                                              observed_preparation->path,
                                                                              observed_preparation->first_load };
        }
        if (!calls_.emplace(entry, std::move(call)).second) throw std::runtime_error("Allocator calls share an entry boundary");
        fact_entries_.emplace(selected[i].fact->source_node_id, entry);
        owned_.insert(region.work.caller.owned_node_ids.begin(), region.work.caller.owned_node_ids.end());
        owned_.insert(region.work.submitted_nodes.begin(), region.work.submitted_nodes.end());
    }
    (void)core::apply_dag_mutation_plan(graph, plan_host_removal(source, queues, removals));
}

void HiCacheAllocatorCalls::rebind_resources(const adapters::sglang::HiCacheSourceIndex & source, const std::map<size_t, size_t> & workers) {
    std::vector<HiCacheHostExpansion *> retained;
    for (auto & [entry, call] : calls_) {
        retained.push_back(&call.retained);
        call.resources.worker = workers.at(source.graph().node(call.resources.worker).lane_id);
        auto resources = call.retained.resource_nodes();
        resources.push_back(call.resources.compute);
        for (const auto resource : resources) {
            const auto lane = source.graph().node(resource).lane_id;
            if (!call.positions.contains(lane))
                call.positions.emplace(lane, observe_write_stream_position(source, resource, call.pid, call.tid, call.begin_us));
        }
    }
    rebind_host_worker_queues(source.graph(), workers, retained);
}

void HiCacheAllocatorCalls::set_work(const model::HiCacheAllocatorWorkItem & work, const adapters::sglang::AllocatorPreparation & preparation) {
    if (!work.formal) return;
    auto & call = calls_.at(fact_entries_.at(work.source_fact_id));
    call.work = work;
    call.preparation = preparation;
}

void HiCacheAllocatorCalls::advance(core::DagGraph & graph, size_t node, simulation::FutureDag & future) {
    const auto found = calls_.find(node);
    if (found == calls_.end()) return;
    auto & call = found->second;
    const auto & work = call.work.value();
    const bool decode = work.phase == model::HiCacheAllocatorPhase::Decode;
    bool preparation_unchanged = true;
    if (!decode) {
        const auto & source_preparation = call.source_preparation.value();
        const bool required = call.preparation.status == "required";
        preparation_unchanged = required == (source_preparation.status == "required")
                                && (!required
                                    || (call.preparation.specialization == source_preparation.specialization && call.preparation.path == source_preparation.path
                                        && call.preparation.first_load == source_preparation.first_load));
    }
    const bool unchanged = preparation_unchanged && call.source_work && adapters::sglang::allocator_work_unchanged(*call.source_work, work);
    HiCacheHostExpansion plan;
    if (unchanged) {
        plan = std::move(call.retained);
        ++retained_;
    }
    else {
        // Retaining proven unchanged source work does not require a native
        // lowering rule. New work does: an unknown device must not silently
        // inherit the inspected 910B implementation.
        std::optional<std::vector<structure::TensorOperation>> program;
        if (decode) { program = adapters::sglang::ascend_decode_allocator_program(work, call.inputs); }
        else if (call.preparation.status == "naive_path") {
            program = adapters::sglang::ascend_naive_extend_allocator_program(work, call.inputs, call.prefix_dtype.value(), call.page_count_supplied.value());
        }
        else {
            program = adapters::sglang::ascend_fused_extend_allocator_program(work,
                                                                              call.inputs,
                                                                              call.prefix_dtype.value(),
                                                                              call.page_count_supplied.value(),
                                                                              call.preparation);
        }
        if (!program)
            throw std::runtime_error("Target allocator has no implemented program for its device/dtype/execution inputs; device_name="
                                     + (call.inputs.device_name.empty() ? std::string("unknown") : call.inputs.device_name));

        auto generated = generate_tensor_program(graph, node, call.resources.worker, call.resources.compute, *program, "allocator");
        bind_host_operations(generated.plan, generated.operations, provider_, "tensor_independent_model");
        plan = std::move(generated.plan);
    }
    nlohmann::json requests = nlohmann::json::array();
    for (const auto & request : work.requests)
        requests.push_back({
            { "request_id",                                                                             request.request_id },
            {  "iteration", request.decode_iteration ? nlohmann::json(*request.decode_iteration) : nlohmann::json(nullptr) }
        });
    const auto request_identity = requests.dump();
    for (auto & item : plan.nodes) {
        item.work.attrs["allocator_requests"] = request_identity;
        item.work.attrs["allocator_pid"] = call.pid;
        item.work.attrs["allocator_phase"] = decode ? "decode" : "extend";
        item.work.attrs["allocator_action"] = unchanged ? "retained" : "generated";
    }

    HiCacheHostSequence sequence(call.positions, insertions_, future, node);
    const auto lanes = plan.resource_lanes(graph);
    const auto expanded = sequence.append(plan, lanes);
    future.depend(expanded.host_return, call.exit, core::DagEdgeKind::Sequential);
    ++executed_;
}

} // namespace markov::trace_graph::modules::hicache::runtime
