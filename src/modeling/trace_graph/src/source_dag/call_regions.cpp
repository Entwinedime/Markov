#include "markov/trace_graph/source_dag/call_regions.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include "markov/trace_graph/source_dag/operator_envelopes.hpp"

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace markov::trace_graph::source_dag {

std::optional<std::string_view> native_launch_label(std::string_view implementation) {
    static const std::map<std::string_view, std::string_view> labels{
        {    "ascend.argmax.float32_to_int32.aicore",               "_ArgMaxV2AiCore_ArgMaxV2" },
        {               "ascend.cast.int32_to_int64",                       "_CastAiCore_Cast" },
        {                "ascend.cast.bool_to_int64",                       "_CastAiCore_Cast" },
        {               "ascend.cast.int64_to_int32",                       "_CastAiCore_Cast" },
        {                "ascend.slice.int32.aicore",                     "_SliceAiCore_Slice" },
        {             "ascend.slice.bfloat16.aicore",                     "_SliceAiCore_Slice" },
        { "ascend.scatter_nd_update.bfloat16.aicore", "_ScatterNdUpdateAiCore_ScatterNdUpdate" },
        {                "ascend.index.int32.aicore",                     "_IndexAiCore_Index" },
        {           "ascend.as_strided.int32.aicore",             "_AsStridedAiCore_AsStrided" },
        {                "ascend.cumsum.int64.aicpu",                    "_CumsumAiCpu_Cumsum" },
        {                  "ascend.add.int64.aicore",                         "_AddAiCore_Add" },
        {                  "ascend.sub.int64.aicore",                         "_SubAiCore_Sub" },
        {                  "ascend.mul.int64.aicore",                         "_MulAiCore_Mul" },
        {         "ascend.floor_divide.int64.aicore",               "_FloorDivAiCore_FloorDiv" },
        {            "ascend.broadcast.int64.aicore",         "_BroadcastToAiCore_BroadcastTo" },
        {            "ascend.floor_mod.int64.aicore",               "_FloorModAiCore_FloorMod" },
    };
    const auto found = labels.find(implementation);
    return found == labels.end() ? std::nullopt : std::optional{ found->second };
}

std::set<size_t> interior_stream_placeholders(const modules::hicache::patch::HiCacheSourceDagIndex & source, const std::set<size_t> & owned) {
    const auto & graph = source.graph();
    std::set<size_t> result;
    for (const auto start : owned) {
        if (graph.node(start).is_cpu) continue;

        auto current = start;
        std::vector<size_t> interior;
        while (true) {
            std::optional<size_t> next;
            bool branching = false;
            for (const auto id : source.outgoing_edge_ids(current)) {
                const auto & edge = graph.edge(id);
                if (!edge.active || edge.kind != core::DagEdgeKind::Stream) continue;
                if (next) branching = true;
                next = edge.dst;
            }
            if (branching || !next || !graph.node(*next).active || graph.node(*next).lane_id != graph.node(start).lane_id) break;
            if (owned.contains(*next)) {
                result.insert(interior.begin(), interior.end());
                break;
            }

            const auto & node = graph.node(*next);
            const auto & event = graph.event_for_node(*next);
            if (node.is_cpu || node.duration || event.dur || event.name != "PLACE_HOLDER_SQE") break;

            size_t predecessors = 0;
            bool external = false;
            for (const auto id : source.incoming_edge_ids(*next)) {
                const auto & edge = graph.edge(id);
                if (!edge.active) continue;
                external |= edge.kind != core::DagEdgeKind::Stream || edge.src != current;
                ++predecessors;
            }
            for (const auto id : source.outgoing_edge_ids(*next)) {
                const auto & edge = graph.edge(id);
                external |= edge.active && edge.kind != core::DagEdgeKind::Stream;
            }
            if (external || predecessors != 1) break;

            interior.push_back(*next);
            current = *next;
        }
    }
    return result;
}

namespace {


std::optional<std::string> torch_operator_name(std::string_view implementation) {
    // Actual framework schemas, not native kernel name heuristics. Layout
    // and dtype stay in the semantic implementation identity.
    if (implementation.starts_with("aten._to_copy:")) return "aten::_to_copy";
    if (implementation == "aten.to.cpu" || implementation.starts_with("aten.to.cpu:")) return "aten::_to_copy";
    if (implementation.starts_with("aten.max:")) return "aten::max";
    if (implementation.starts_with("aten._local_scalar_dense:")) return "aten::_local_scalar_dense";
    if (implementation == "aten.index:page_table_int32") return "aten::index";
    if (implementation.starts_with("aten.index:logits_hidden:")) return "aten::index";
    if (implementation.starts_with("aten.cumsum:")) return "aten::cumsum";
    if (implementation.starts_with("torch._foreach_copy_:")) return "aten::_foreach_copy_";
    if (implementation == "aten.floor_divide:page_table_int32") return "aten::floor_divide";
    if (implementation == "aten.fill_:int32") return "aten::fill_";
    if (implementation.starts_with("torch.linear:")) return "aten::linear";
    if (implementation.starts_with("torch.matmul.logits:")) return "aten::matmul";
    if (implementation.starts_with("torch.embedding:")) return "aten::embedding";
    if (implementation.starts_with("aten.masked_fill_:embedding:")) return "aten::masked_fill_";
    if (implementation.starts_with("aten.argmax:")) return "aten::argmax";
    if (implementation.starts_with("torch_npu.swiglu:")) return "npu::npu_swiglu";
    if (implementation.starts_with("torch_npu.rms_norm:")) return "npu::npu_rms_norm";
    if (implementation.starts_with("torch_npu.add_rms_norm:")) return "npu::npu_add_rms_norm";
    if (implementation.starts_with("sgl_kernel_npu.split_qkv_rmsnorm_rope:")) return "runtime.phase.kernel";
    if (implementation.starts_with("aten.index_select:")) return "aten::index_select";
    if (implementation.starts_with("aten.repeat:")) return "aten::repeat";
    if (implementation.starts_with("aten.clone:")) return "aten::clone";
    for (const auto name : { "mul", "sub", "add", "cat", "stack", "ge", "lt", "bitwise_and", "bitwise_or", "bitwise_not" })
        if (implementation.starts_with(std::string("aten.") + name + ":")) return std::string("aten::") + name;
    if (implementation.starts_with("torch_npu.mrope:")) return "npu::npu_mrope";
    if (implementation == "aten.copy_" || implementation.starts_with("aten.copy_:")) return "aten::copy_";
    if (implementation == "aten.zero_") return "aten::zero_";
    if (implementation == "aten.cat") return "aten::cat";
    if (implementation.starts_with("ascend.fia_v2.")) return "npu::npu_fused_infer_attention_score_v2";
    if (implementation.starts_with("ascend.fia.batch_invariant.")) return "batch_invariant_ops::npu_fused_infer_attention_score_batch_invariant";
    if (implementation.starts_with("ascend.fia.")) return "npu::npu_fused_infer_attention_score";
    if (implementation == "ascend.flash_attention_qlens") return "npu::_npu_flash_attention_qlens";
    if (implementation == "ascend.paged_attention") return "npu::_npu_paged_attention";
    if (implementation == "ascend.scatter_nd_update_") return "npu::npu_scatter_nd_update_";
    if (implementation == "ascend.reshape_and_cache") return "npu::_npu_reshape_and_cache";
    return std::nullopt;
}

} // namespace

std::map<size_t, size_t> match_tensor_operators(const modules::hicache::patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                                std::span<const size_t> operator_observations, std::span<const structure::TensorOperation> program) {
    std::map<size_t, std::string> expected;
    for (size_t i = 0; i < program.size(); ++i) {
        const auto execution = program[i].execution;
        if (execution != structure::TensorExecutionKind::AsyncDevice && execution != structure::TensorExecutionKind::DirectBlockingDevice
            && execution != structure::TensorExecutionKind::BlockingContextReadback)
            continue;
        if (const auto name = torch_operator_name(program[i].work.implementation)) expected.emplace(i, *name);
    }
    return match_submitted_operator_envelopes(source, queues, operator_observations, expected);
}

std::map<size_t, size_t> match_submitted_operator_envelopes(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                            const simulation::detail::CpuTaskQueues & queues, std::span<const size_t> observations,
                                                            const std::map<size_t, std::string> & expected) {
    const auto & graph = source.graph();
    std::set<std::string> schemas;
    for (const auto & [position, schema] : expected) schemas.insert(schema);

    struct Candidate {
        size_t observation;
        uint64_t begin_ns, end_ns;
        std::set<size_t> devices;
    };
    std::vector<Candidate> candidates;
    for (const auto observation : observations) {
        if (!schemas.contains(graph.runtime_observations().at(observation).name)) continue;
        const auto region = observe_submitted_call_region(source, queues, observation);
        if (region.caller.has_node_overlap) continue;
        const auto & schema = graph.runtime_observations().at(observation).name;
        const bool readback = (schema == "aten::_local_scalar_dense" || schema == "aten::_to_copy") && observe_synchronous_readback(source, region).has_value();
        std::set<size_t> devices;
        for (const auto node : region.submitted_nodes)
            if (!graph.node(node).is_cpu) devices.insert(node);
        if (!readback && devices.empty()) continue;

        const auto & event = graph.runtime_observations().at(observation);
        candidates.push_back({ observation, event.start_ns(), event.end_ns(), std::move(devices) });
    }

    std::vector<size_t> submitted;
    for (const auto & candidate : candidates) {
        const auto & event = graph.runtime_observations().at(candidate.observation);
        // Composite calls (stack -> cat) and backend redispatch (argmax ->
        // argmax) can expose nested envelopes for exactly the same device
        // work. Keep its enclosing operation once. Strict containment and
        // identical device ownership are required, not just matching names;
        // equal envelopes and calls with different work remain unresolved.
        const bool nested_alias = !candidate.devices.empty() && std::ranges::any_of(candidates, [&](const auto & parent) {
            const auto & outer = graph.runtime_observations().at(parent.observation);
            return outer.pid == event.pid && outer.tid == event.tid && parent.begin_ns <= candidate.begin_ns && parent.end_ns >= candidate.end_ns
                   && (parent.begin_ns < candidate.begin_ns || parent.end_ns > candidate.end_ns) && parent.devices == candidate.devices;
        });
        if (!nested_alias) submitted.push_back(candidate.observation);
    }
    return match_operator_envelopes(graph, submitted, expected);
}

std::map<size_t, size_t> match_operator_envelopes(const core::DagGraph & graph, std::span<const size_t> observations,
                                                  const std::map<size_t, std::string> & schemas) {
    std::map<std::string, std::vector<size_t>> expected, observed;
    for (const auto & [position, name] : schemas) expected[name].push_back(position);
    for (const auto index : observations) {
        const auto & event = graph.runtime_observations().at(index);
        if (expected.contains(event.name)) observed[event.name].push_back(index);
    }

    std::map<size_t, size_t> matched;
    for (const auto & [name, operations] : expected) {
        const auto found = observed.find(name);
        // Ordered repeated writes can be paired only when the whole family
        // agrees. Extra nested casts/copies make attribution ambiguous.
        if (found == observed.end() || found->second.size() != operations.size()) continue;
        for (size_t i = 0; i < operations.size(); ++i) matched.emplace(operations[i], found->second[i]);
    }
    return matched;
}

std::optional<DeviceChain> observe_serial_device_chain(const modules::hicache::patch::HiCacheSourceDagIndex & source, const std::set<size_t> & devices) {
    if (devices.empty()) return std::nullopt;
    const auto & graph = source.graph();
    const auto lane = graph.node(*devices.begin()).lane_id;
    DeviceChain result{ *devices.begin(), *devices.begin() };
    size_t links = 0;
    for (const auto node : devices) {
        if (graph.node(node).lane_id != lane) return std::nullopt;
        size_t incoming = 0, outgoing = 0;
        for (const auto edge_id : source.incoming_edge_ids(node)) {
            const auto & edge = graph.edge(edge_id);
            if (!edge.active) continue;
            if (edge.kind == core::DagEdgeKind::Sync) return std::nullopt;
            incoming += edge.kind == core::DagEdgeKind::Stream && devices.contains(edge.src);
        }
        for (const auto edge_id : source.outgoing_edge_ids(node)) {
            const auto & edge = graph.edge(edge_id);
            if (edge.active && edge.kind == core::DagEdgeKind::Stream && devices.contains(edge.dst)) ++outgoing;
        }
        if (incoming > 1 || outgoing > 1) return std::nullopt;
        if (!incoming) result.first = node;
        if (!outgoing) result.last = node;
        links += outgoing;
    }
    // In an acyclic graph, N-1 links and degree at most one prove one chain.
    if (links + 1 != devices.size()) return std::nullopt;
    return result;
}

SubmittedCallRegion observe_submitted_call_region(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                  const simulation::detail::CpuTaskQueues & queues, size_t observation) {
    const auto & graph = source.graph();
    const auto & event = graph.runtime_observations().at(observation);
    SubmittedCallRegion region{ .observation = observation, .caller = source.call_interval_ownership(event.pid, event.tid, event.ts, event.dur) };
    // A partial CPU boundary is not enough to claim downstream work.
    if (region.caller.status != "ready" && region.caller.status != "zero_duration") return region;

    std::set<size_t> owned(region.caller.owned_node_ids.begin(), region.caller.owned_node_ids.end());
    auto pending = region.caller.owned_node_ids;
    for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
        for (const auto edge_id : source.outgoing_edge_ids(pending[cursor])) {
            const auto & edge = graph.edge(edge_id);
            if (!graph.node(edge.dst).active) continue;
            const auto task = queues.node_task.at(edge.src);
            const bool same_task = edge.kind == core::DagEdgeKind::Sequential && task != core::DagNode::kNoNode && queues.node_task.at(edge.dst) == task
                                   && owned.contains(queues.tasks.at(task).submission);
            if (edge.kind != core::DagEdgeKind::Correlation && !same_task) continue;
            if (!owned.insert(edge.dst).second) continue;

            region.submitted_nodes.push_back(edge.dst);
            pending.push_back(edge.dst);
        }
    }
    for (const auto node : pending) {
        for (const auto edge_id : source.incoming_edge_ids(node)) {
            const auto & edge = graph.edge(edge_id);
            if (!owned.contains(edge.src)) region.incoming_edges.push_back(edge_id);
        }
        for (const auto edge_id : source.outgoing_edge_ids(node))
            if (!owned.contains(graph.edge(edge_id).dst)) region.outgoing_edges.push_back(edge_id);
    }
    std::ranges::sort(region.submitted_nodes);
    return region;
}

std::optional<std::vector<QueuedCallTask>> observe_queued_call_tasks(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                                     const simulation::detail::CpuTaskQueues & queues, const SubmittedCallRegion & region) {
    if (region.caller.status != "ready" || region.caller.has_node_overlap) return std::nullopt;
    const auto & graph = source.graph();
    const std::set<size_t> caller(region.caller.owned_node_ids.begin(), region.caller.owned_node_ids.end());
    std::map<size_t, QueuedCallTask> tasks;
    std::set<size_t> devices, attributed;
    for (const auto node : region.submitted_nodes) {
        if (!graph.node(node).is_cpu) {
            devices.insert(node);
            continue;
        }
        const auto task = queues.node_task.at(node);
        if (task == core::DagNode::kNoNode || !caller.contains(queues.tasks.at(task).submission)) return std::nullopt;
        tasks.try_emplace(task, QueuedCallTask{ task, {}, {} }).first->second.workers.push_back(node);
    }

    std::vector<QueuedCallTask> result;
    for (auto & [task_id, observed] : tasks) {
        const auto & task = queues.tasks.at(task_id);
        const std::set<size_t> workers(observed.workers.begin(), observed.workers.end());
        if (!workers.contains(task.first) || !workers.contains(task.last)) return std::nullopt;
        for (const auto worker : workers) {
            for (const auto edge_id : source.outgoing_edge_ids(worker)) {
                const auto & edge = graph.edge(edge_id);
                if (edge.kind == core::DagEdgeKind::Sequential && queues.node_task.at(edge.dst) == task_id && !workers.contains(edge.dst)) return std::nullopt;
                if (edge.kind != core::DagEdgeKind::Correlation || graph.node(edge.dst).is_cpu) continue;
                if (!devices.contains(edge.dst) || !attributed.insert(edge.dst).second) return std::nullopt;
                observed.devices.insert(edge.dst);
            }
        }
        std::ranges::sort(observed.workers, {}, [&](size_t node) { return std::pair{ graph.event_for_node(node).ts, node }; });
        result.push_back(std::move(observed));
    }
    if (attributed != devices) return std::nullopt;

    std::ranges::sort(result, {}, [&](const auto & task) {
        const auto submission = queues.tasks.at(task.task).submission;
        return std::pair{ graph.event_for_node(submission).ts, submission };
    });
    return result;
}

std::optional<SynchronousReadback> observe_synchronous_readback(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                                const SubmittedCallRegion & region) {
    if (region.caller.status != "ready" || region.caller.has_node_overlap) return std::nullopt;
    const auto & graph = source.graph();
    std::optional<size_t> sync, copy;
    for (const auto node : region.caller.owned_node_ids) {
        const auto & event = graph.event_for_node(node);
        if (event.name == "AscendCL@aclrtSynchronizeStream" || event.name == "AscendCL@aclrtSynchronizeStreamWithTimeout") {
            if (sync || copy) return std::nullopt;
            sync = node;
        }
        else if (event.name == "AscendCL@aclrtMemcpy") {
            if (!sync || copy) return std::nullopt;
            copy = node;
        }
        else if (core::synchronizes_recorded_event(event) || core::synchronizes_device_frontier(event)) return std::nullopt;

        for (const auto edge_id : source.outgoing_edge_ids(node)) {
            const auto & edge = graph.edge(edge_id);
            if (edge.kind == core::DagEdgeKind::Correlation && (!copy || node != *copy || graph.node(edge.dst).is_cpu)) return std::nullopt;
        }
    }
    if (!sync || !copy) return std::nullopt;

    std::optional<size_t> completion;
    for (const auto edge_id : source.incoming_edge_ids(*sync)) {
        const auto & edge = graph.edge(edge_id);
        if (edge.kind != core::DagEdgeKind::Sync) continue;
        const auto & producer = graph.node(edge.src);
        if (producer.is_cpu || producer.kind != core::DagNodeKind::TraceEvent || completion) return std::nullopt;
        completion = edge.src;
    }
    if (!completion) return std::nullopt;
    return SynchronousReadback{ *sync, region.caller.owned_node_ids.back(), *completion };
}

std::vector<AllocatorRegion> observe_allocator_regions(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                       const simulation::detail::CpuTaskQueues & queues) {
    const auto & graph = source.graph();
    std::vector<AllocatorRegion> regions;
    const auto & observations = graph.runtime_observations();
    const detail::OperatorEnvelopeIndex operators(observations);
    for (size_t i = 0; i < observations.size(); ++i) {
        const auto & event = observations[i];
        if (event.name != "runtime.hicache.extend_allocator" && event.name != "runtime.hicache.decode_allocator") continue;
        if (event.arg("status") != "returned") continue;

        std::optional<bool> allocated;
        if (event.arg("allocated") == "true") allocated = true;
        else if (event.arg("allocated") == "false") allocated = false;

        regions.push_back({
            .work = observe_submitted_call_region(source, queues, i),
            .phase = event.name == "runtime.hicache.extend_allocator" ? modules::hicache::model::HiCacheAllocatorPhase::Extend
                                                                      : modules::hicache::model::HiCacheAllocatorPhase::Decode,
            .allocated = allocated,
            .operator_observations = operators.inside(event),
        });
    }
    return regions;
}

std::optional<structure::hicache::AscendAllocatorInputs> observed_allocator_inputs(const core::TraceEvent & event) {
    if (event.arg("allocator_implementation") != "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator"
        || !event.has_arg_key_hint("sequence_dtype") || !event.has_arg_key_hint("last_location_dtype") || !event.has_arg_key_hint("free_page_dtype")
        || (event.arg("debug_memory_pool") != "true" && event.arg("debug_memory_pool") != "false"))
        return std::nullopt;

    structure::hicache::AscendAllocatorInputs inputs{
        event.arg("sequence_dtype"),
        event.arg("last_location_dtype"),
        event.arg("free_page_dtype"),
        event.arg("debug_memory_pool") == "true",
    };
    const auto task_queue = event.arg("triton_task_queue_enabled");
    inputs.device_name = event.arg("device_name");
    if (task_queue == "true" || task_queue == "false") inputs.triton_task_queue_enabled = task_queue == "true";
    return inputs;
}

std::map<std::string, structure::hicache::AscendAllocatorInputs> observe_allocator_inputs(const core::DagGraph & graph) {
    std::map<std::string, structure::hicache::AscendAllocatorInputs> result;
    std::set<std::string> ambiguous;
    for (const auto & event : graph.runtime_observations()) {
        if (event.name != "runtime.hicache.decode_allocator") continue;
        const auto observed = observed_allocator_inputs(event);
        if (!observed) {
            ambiguous.insert(event.pid);
            continue;
        }

        const auto & input = *observed;
        const auto [position, added] = result.emplace(event.pid, input);
        const auto & previous = position->second;
        if (!added
            && (previous.sequence_dtype != input.sequence_dtype || previous.last_location_dtype != input.last_location_dtype
                || previous.free_page_dtype != input.free_page_dtype || previous.debug_memory_pool != input.debug_memory_pool
                || previous.device_name != input.device_name))
            ambiguous.insert(event.pid);
    }
    for (const auto & pid : ambiguous) result.erase(pid);
    return result;
}

std::optional<modules::hicache::model::HiCacheAllocatorWorkItem> observed_allocator_work(const core::TraceEvent & event) {
    using namespace modules::hicache::model;
    const bool decode = event.name == "runtime.hicache.decode_allocator";
    if ((!decode && event.name != "runtime.hicache.extend_allocator") || event.arg("status") != "returned" || event.arg("allocated") != "true")
        return std::nullopt;
    if (!decode && !event.has_arg_key_hint("extend_tokens")) return std::nullopt;
    for (const auto * field : { "page_size_tokens", "batch_size", "free_pages_before", "release_pages_before", "free_pages_after", "release_pages_after" })
        if (!event.has_arg_key_hint(field)) return std::nullopt;

    const auto free_before = event.arg_u64("free_pages_before"), release_before = event.arg_u64("release_pages_before");
    const auto free_after = event.arg_u64("free_pages_after"), release_after = event.arg_u64("release_pages_after");
    const auto before = core::checked_add_u64(free_before, release_before, "Observed allocator capacity overflow");
    const auto after = core::checked_add_u64(free_after, release_after, "Observed allocator capacity overflow");
    if (after > before || (release_after != 0 && release_after != release_before)) return std::nullopt;
    HiCacheAllocatorWorkItem work{
        .phase = decode ? HiCacheAllocatorPhase::Decode : HiCacheAllocatorPhase::Extend,
        .pid = event.pid,
        .page_size = event.arg_u64("page_size_tokens"),
        .batch_size = event.arg_u64("batch_size"),
        .extend_tokens = event.arg_u64(decode ? "batch_size" : "extend_tokens"),
        .allocated_pages = before - after,
        .queue_state_known = true,
        .free_pages_before = free_before,
        .release_pages_before = release_before,
        .merge_release_queue = release_before > 0 && release_after == 0,
    };
    if (!work.page_size || !work.batch_size || (decode && work.allocated_pages > work.batch_size)) return std::nullopt;
    if (work.merge_release_queue) work.free_index_offset = 0;
    else if (event.has_arg_key_hint("free_page_storage_offset_elements")) work.free_index_offset = event.arg_u64("free_page_storage_offset_elements");
    return work;
}

std::vector<AttentionRegion> observe_attention_regions(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                       const simulation::detail::CpuTaskQueues & queues) {
    const auto & graph = source.graph();
    using Key = std::tuple<std::string, std::string, std::string>;
    std::map<Key, AttentionDescriptorPosition> descriptors;
    const auto remember = [&](const core::TraceEvent & event, AttentionDescriptorPosition position) {
        if (event.name == "runtime.phase.attention" && event.arg("descriptor_present") == "true")
            descriptors.emplace(Key{ event.pid, event.arg("phase"), event.arg("descriptor_id") }, position);
    };
    for (size_t i = 0; i < graph.prelude_context_events().size(); ++i) remember(graph.prelude_context_events()[i], { true, i });

    std::vector<AttentionRegion> regions;
    const auto & events = graph.runtime_observations();
    const detail::OperatorEnvelopeIndex operators(events);
    for (size_t i = 0; i < events.size(); ++i) { remember(events[i], { false, i }); }

    for (size_t i = 0; i < events.size(); ++i) {
        const auto & event = events[i];
        if (event.name != "runtime.phase.attention" || event.arg("formal_window_context") == "causal_head") continue;

        std::optional<AttentionDescriptorPosition> descriptor;
        if (event.has_arg_key_hint("backend_implementation")) descriptor = AttentionDescriptorPosition{ false, i };
        else {
            const auto found = descriptors.find({ event.pid, event.arg("phase"), event.arg("descriptor_id") });
            if (found != descriptors.end()) descriptor = found->second;
        }
        regions.push_back({ observe_submitted_call_region(source, queues, i), event.arg("phase"), event.arg("sampling"), event.arg("status"), descriptor });
        regions.back().operator_observations = operators.inside(event);
    }
    return regions;
}

std::vector<AttentionMetadataRegion> observe_attention_metadata_regions(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                                        const simulation::detail::CpuTaskQueues & queues) {
    const auto & events = source.graph().runtime_observations();
    const detail::OperatorEnvelopeIndex operators(events);
    std::vector<AttentionMetadataRegion> regions;
    for (size_t i = 0; i < events.size(); ++i) {
        const auto & event = events[i];
        if (event.source_channel != core::TraceSourceChannel::PythonProbe || event.name != "runtime.phase.attention_metadata") continue;
        if (event.arg("formal_window_context") == "causal_head") continue;

        regions.push_back({ observe_submitted_call_region(source, queues, i), operators.inside(event) });
    }
    return regions;
}


std::optional<structure::hicache::AscendAttentionConditions> attention_conditions(const core::TraceEvent & descriptor) {
    const auto fields = nlohmann::json::parse(descriptor.args_json_view());
    for (const auto key : { "backend_implementation",
                            "phase",
                            "dllm_model",
                            "topk_present",
                            "mode_is_target_verify",
                            "mode_is_draft_extend",
                            "use_mla",
                            "graph_mode",
                            "enable_torch_compile",
                            "sinks_present",
                            "hybrid_swa",
                            "swa_layer",
                            "mode_is_context_parallel_extend",
                            "cp_metadata_present",
                            "attention_cp_size",
                            "use_fia",
                            "causal_attention",
                            "encoder_present",
                            "use_native_sdpa",
                            "use_alibi",
                            "logit_cap",
                            "query_key_head_dim",
                            "value_head_dim" })
        if (!fields.contains(key)) return std::nullopt;
    if (fields.at("backend_implementation") != "sglang.srt.hardware_backend.npu.attention.ascend_backend.AscendAttnBackend"
        || (fields.at("phase") != "prefill" && fields.at("phase") != "decode"))
        return std::nullopt;

    const auto flag = [&](const char * key) { return fields.at(key).get<bool>(); };
    std::optional<bool> alibi;
    if (!fields.at("use_alibi").is_null()) alibi = flag("use_alibi");
    std::optional<bool> layer_counter;
    std::optional<uint64_t> layer_index;
    std::string pool_implementation;
    std::optional<bool> fia_batch_invariant;
    if (fields.contains("fia_torch_ops_dispatch")) {
        if (fields.at("fia_torch_ops_dispatch") == "native") fia_batch_invariant = false;
        else if (fields.at("fia_torch_ops_dispatch") == "batch_invariant") fia_batch_invariant = true;
    }
    if (fields.contains("cache_write")) {
        const auto & write = fields.at("cache_write");
        if (write.contains("pool_implementation")) pool_implementation = write.at("pool_implementation").get<std::string>();
        if (write.contains("layer_counter_registered")) layer_counter = write.at("layer_counter_registered").get<bool>();
        if (write.contains("layer_index") && !write.at("layer_index").is_null()) {
            const auto index = write.at("layer_index").get<int64_t>();
            if (index >= 0) layer_index = static_cast<uint64_t>(index);
        }
    }
    return structure::hicache::AscendAttentionConditions{
        .prefill = fields.at("phase") == "prefill",
        .dllm = flag("dllm_model"),
        .sparse = flag("topk_present"),
        .verify = flag("mode_is_target_verify"),
        .draft = flag("mode_is_draft_extend"),
        .mla = flag("use_mla"),
        .graph = flag("graph_mode"),
        .compile = flag("enable_torch_compile"),
        .sinks = flag("sinks_present"),
        .hybrid_swa = flag("hybrid_swa"),
        .swa_layer = flag("swa_layer"),
        .context_parallel = flag("mode_is_context_parallel_extend") && flag("cp_metadata_present") && fields.at("attention_cp_size").get<uint64_t>() > 1,
        .fia = flag("use_fia"),
        .causal = flag("causal_attention"),
        .encoder = flag("encoder_present"),
        .native_sdpa = flag("use_native_sdpa"),
        .alibi = alibi,
        .logit_cap = fields.at("logit_cap").get<double>(),
        .query_key_dim = fields.at("query_key_head_dim").get<uint64_t>(),
        .value_dim = fields.at("value_head_dim").get<uint64_t>(),
        .sequence_lengths_on_cpu =
            fields.contains("sequence_lengths_on_cpu") ? std::optional<bool>{ fields.at("sequence_lengths_on_cpu").get<bool>() } : std::nullopt,
        .layer_counter_registered = layer_counter,
        .cache_layer_index = layer_index,
        .cache_pool_implementation = std::move(pool_implementation),
        .torch_ops_fia_batch_invariant = fia_batch_invariant,
    };
}

std::optional<structure::hicache::AttentionBatchShape> attention_batch_shape(const core::TraceEvent & descriptor,
                                                                             std::optional<uint64_t> observed_decode_context) {
    const auto fields = nlohmann::json::parse(descriptor.args_json_view());
    for (const auto key : { "phase", "query_shape", "batch_size", "query_heads", "key_heads", "page_size_tokens" })
        if (!fields.contains(key) || fields.at(key).is_null()) return std::nullopt;

    const auto dimensions = fields.at("query_shape").get<std::vector<int64_t>>();
    const auto batch_size = fields.at("batch_size").get<size_t>();
    // This is an external trace boundary. Incomplete/truncated lists cannot
    // establish a different (smaller) batch or an unsigned wrapped token count.
    if (dimensions.empty() || dimensions.front() < 0) return std::nullopt;

    structure::hicache::AttentionBatchShape result{
        .padded_query_tokens = static_cast<uint64_t>(dimensions.front()),
        .query_heads = fields.at("query_heads").get<uint64_t>(),
        .kv_heads = fields.at("key_heads").get<uint64_t>(),
        .page_tokens = fields.at("page_size_tokens").get<uint64_t>(),
    };
    if (fields.at("phase") == "decode") {
        if (batch_size != 1 || !observed_decode_context || result.padded_query_tokens < 1) return std::nullopt;
        result.requests.push_back({ 1, *observed_decode_context });
        return result;
    }
    if (fields.at("phase") != "prefill") return std::nullopt;
    for (const auto key : { "extend_query_lengths", "extend_prefix_lengths" })
        if (!fields.contains(key) || fields.at(key).is_null()) return std::nullopt;

    const auto queries = fields.at("extend_query_lengths").get<std::vector<int64_t>>();
    const auto prefixes = fields.at("extend_prefix_lengths").get<std::vector<int64_t>>();
    if (queries.size() != batch_size || prefixes.size() != batch_size) return std::nullopt;

    uint64_t total_query_tokens = 0;
    for (size_t i = 0; i < batch_size; ++i) {
        if (queries[i] < 0 || prefixes[i] < 0) return std::nullopt;

        const auto query = static_cast<uint64_t>(queries[i]);
        if (query > result.padded_query_tokens - total_query_tokens) return std::nullopt;
        total_query_tokens += query;
        result.requests.push_back({ query, query + static_cast<uint64_t>(prefixes[i]) });
    }
    return result;
}

std::optional<DecodeOperationContext> attention_decode_context(const AttentionRegion & region, const DecodeOperatorContexts & contexts) {
    if (region.phase != "decode" || region.status != "returned") return std::nullopt;

    std::optional<DecodeOperationContext> result;
    for (const auto * nodes : { &region.work.caller.owned_node_ids, &region.work.submitted_nodes }) {
        for (const auto node : *nodes) {
            const auto found = contexts.find(node);
            if (found == contexts.end()) continue;
            if (result && *result != found->second) return std::nullopt;
            result = found->second;
        }
    }
    return result;
}

std::optional<structure::hicache::AttentionCacheWrite> attention_cache_write(const core::TraceEvent & descriptor) {
    const auto fields = nlohmann::json::parse(descriptor.args_json_view());
    for (const auto key : { "save_kv_cache", "key_present", "value_present", "phase", "graph_mode", "enable_torch_compile" })
        if (!fields.contains(key)) return std::nullopt;
    if (!fields.at("save_kv_cache").get<bool>()) return structure::hicache::AttentionCacheWrite{ .enabled = false };

    const bool graph_decode = fields.at("phase") == "decode" && fields.at("graph_mode").get<bool>() && !fields.at("enable_torch_compile").get<bool>();
    if (!graph_decode && (!fields.at("key_present").get<bool>() || !fields.at("value_present").get<bool>()))
        return structure::hicache::AttentionCacheWrite{ .enabled = false };
    if (!fields.contains("cache_write")) return std::nullopt;

    const auto & write = fields.at("cache_write");
    for (const auto key :
         { "pool_implementation", "use_fia", "cache_dtype", "key_dtype", "value_dtype", "location_dtype", "key_elements", "value_elements", "locations" })
        if (!write.contains(key) || write.at(key).is_null()) return std::nullopt;

    return structure::hicache::AttentionCacheWrite{
        .enabled = true,
        .fia = write.at("use_fia").get<bool>(),
        .cache_dtype = write.at("cache_dtype").get<std::string>(),
        .key_dtype = write.at("key_dtype").get<std::string>(),
        .value_dtype = write.at("value_dtype").get<std::string>(),
        .location_dtype = write.at("location_dtype").get<std::string>(),
        .key_elements = write.at("key_elements").get<uint64_t>(),
        .value_elements = write.at("value_elements").get<uint64_t>(),
        .locations = write.at("locations").get<uint64_t>(),
    };
}

std::optional<structure::hicache::AttentionEagerPadding> attention_eager_padding(const core::TraceEvent & descriptor) {
    const auto fields = nlohmann::json::parse(descriptor.args_json_view());
    if (!fields.contains("padding")) return std::nullopt;
    const auto & padding = fields.at("padding");
    for (const auto key : { "mlp_sync", "allow_input_scattered", "tp_size", "can_run_tbo" })
        if (!padding.contains(key) || padding.at(key).is_null()) return std::nullopt;
    const auto tp_size = padding.at("tp_size").get<int64_t>();
    if (tp_size <= 0) return std::nullopt;

    return structure::hicache::AttentionEagerPadding{ padding.at("mlp_sync").get<bool>(),
                                                      padding.at("allow_input_scattered").get<bool>(),
                                                      static_cast<uint64_t>(tp_size),
                                                      padding.at("can_run_tbo").get<bool>() };
}

PhaseCallPositions phase_call_positions(const modules::hicache::HiCachePhaseObservationAudit & phases) {
    PhaseCallPositions positions;
    if (!phases.ready()) return positions;

    const auto record = [&](size_t node, PhaseCallPosition position) {
        const auto [found, added] = positions.nodes.emplace(node, position);
        if (!added && found->second != position) found->second.reset();
    };
    for (size_t i = 0; i < phases.observations.size(); ++i) {
        const auto & phase = phases.observations[i];
        for (const auto * nodes : { &phase.prefill_common_kernel_node_ids,
                                    &phase.prefill_prefix_attention_node_ids,
                                    &phase.prefill.collective_node_ids,
                                    &phase.prefill.submit_cpu_node_ids })
            for (const auto node : *nodes) record(node, { i, std::nullopt });
        for (const auto & [node, iteration] : phase.decode_node_iterations) record(node, { i, iteration });
        for (const auto call : phase.prefill_call_observations) positions.calls.emplace(call, PhaseCallPosition{ i, std::nullopt });
        for (const auto & [call, iteration] : phase.decode_call_iterations) positions.calls.emplace(call, PhaseCallPosition{ i, iteration });
    }
    return positions;
}

std::optional<PhaseCallPosition> call_phase_position(const SubmittedCallRegion & region, const PhaseCallPositions & positions) {
    std::optional<PhaseCallPosition> result;
    for (const auto * nodes : { &region.caller.owned_node_ids, &region.submitted_nodes }) {
        for (const auto node : *nodes) {
            const auto found = positions.nodes.find(node);
            if (found == positions.nodes.end()) continue;
            if (!found->second || (result && result != found->second)) return std::nullopt;
            result = found->second;
        }
    }
    const auto call = positions.calls.find(region.observation);
    if (call != positions.calls.end()) {
        if (result && result != call->second) return std::nullopt;
        if (!result && region.submitted_nodes.empty() && !region.caller.has_node_overlap
            && (region.caller.status == "ready" || region.caller.status == "zero_duration"))
            result = call->second;
    }
    return result;
}

std::optional<structure::hicache::AttentionMetadataInputs> observed_attention_metadata_inputs(const core::TraceEvent & descriptor) {
    if (descriptor.arg("backend_implementation") != "sglang.srt.hardware_backend.npu.attention.ascend_backend.AscendAttnBackend"
        || descriptor.arg("status") != "returned" || descriptor.arg_u64("batch_size") != 1 || descriptor.arg("request_table_dtype") != "torch.int32"
        || descriptor.arg("speculative_present") != "false" || descriptor.arg("mode_is_target_verify") != "false"
        || descriptor.arg("mode_is_draft_extend") != "false" || descriptor.arg("hybrid_swa") != "false" || descriptor.arg("use_mla") != "false"
        || descriptor.arg("in_capture") != "false" || descriptor.arg("sliding_window_kv_pool") != "false" || descriptor.arg("sequence_device_type") != "npu"
        || descriptor.arg("request_indices_device_type") != "npu" || descriptor.arg("request_indices_dtype") != "torch.int64"
        || descriptor.arg("request_table_contiguous") != "true")
        return std::nullopt;

    const auto route = descriptor.arg("route");
    const auto page_tokens = descriptor.arg_u64("page_size_tokens");
    if ((route != "eager" && route != "graph") || page_tokens == 0) return std::nullopt;

    const auto fields = nlohmann::json::parse(descriptor.args_json_view());
    if (!fields.contains("sequence_shape") || fields.at("sequence_shape") != nlohmann::json::array({ 1 }) || !fields.contains("request_indices_shape")
        || fields.at("request_indices_shape") != nlohmann::json::array({ 1 }))
        return std::nullopt;

    const auto sequence_dtype = descriptor.arg("sequence_dtype");
    const auto max_context = descriptor.arg_u64("max_context_tokens");
    const auto context_tokens = descriptor.arg_u64("context_tokens");
    if ((sequence_dtype != "torch.int32" && sequence_dtype != "torch.int64") || context_tokens == 0 || max_context < context_tokens) return std::nullopt;

    // The framework slices all request rows before advanced indexing. Its
    // storage width, not the selected batch size, determines contiguity.
    if (!fields.contains("request_table_shape")) return std::nullopt;
    const auto & table = fields.at("request_table_shape");
    if (!table.is_array() || table.size() != 2 || !table[0].is_number_unsigned() || !table[1].is_number_unsigned()) return std::nullopt;
    const auto table_rows = table[0].get<uint64_t>();
    const auto table_columns = table[1].get<uint64_t>();
    if (table_rows == 0 || table_columns < context_tokens) return std::nullopt;

    structure::hicache::AttentionMetadataInputs result{ route == "graph", context_tokens, page_tokens, max_context, descriptor.arg("device_name"),
                                                        sequence_dtype,   std::nullopt,   false,       table_rows,  table_columns };

    if (result.graph) {
        const auto alias = descriptor.arg("sequence_copy_identity");
        if (alias != "true" && alias != "false") return std::nullopt;
        result.sequence_copy_identity = alias == "true";
        return result;
    }
    if (descriptor.arg("extend_present") == "false") return result;
    if (descriptor.arg("extend_present") != "true") return std::nullopt;

    const auto dtype = descriptor.arg("extend_dtype");
    if (dtype != "torch.int32" && dtype != "torch.int64") return std::nullopt;
    if (descriptor.arg("extend_device_type") == "npu") result.extend_readback_dtype = dtype;
    else if (descriptor.arg("extend_device_type") != "cpu") return std::nullopt;
    return result;
}

DenseCallAudit observe_dense_calls(const modules::hicache::patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                   const modules::hicache::HiCachePhaseObservationAudit & phases, const AttentionCallAudit & attention) {
    DenseCallAudit result;
    const auto positions = phase_call_positions(phases);
    using Identity = std::tuple<size_t, std::optional<uint64_t>, uint64_t>;
    std::map<Identity, std::vector<size_t>> attention_by_layer;
    const auto & graph = source.graph();
    std::set<uint64_t> qwen3_layers, start_layers, output_norm_layers, logits_layers;
    std::map<uint64_t, uint64_t> input_embedding_layers;
    for (const auto & event : graph.runtime_observations()) {
        if ((event.name == "runtime.phase.linear" || event.name == "runtime.phase.gated_activation" || event.name == "runtime.phase.normalization"
             || event.name == "runtime.phase.qkv_norm_rope")
            && event.arg("model_layer_implementation") == "sglang.srt.models.qwen3.Qwen3DecoderLayer" && event.has_arg("model_layer_index")) {
            qwen3_layers.insert(event.arg_u64("model_layer_index"));
            if (event.has_arg("model_start_layer_index")) start_layers.insert(event.arg_u64("model_start_layer_index"));
            if (event.arg("model_output_norm_present") == "true") output_norm_layers.insert(event.arg_u64("model_layer_index"));
            if (event.arg("model_logits_present") == "true") logits_layers.insert(event.arg_u64("model_layer_index"));
            if (event.arg("model_input_embedding_present") == "true")
                input_embedding_layers.emplace(event.arg_u64("model_layer_index"), event.arg_u64("model_input_embedding_tp_size"));
        }
    }
    for (size_t i = 0; i < attention.calls.size(); ++i) {
        const auto & call = attention.calls[i];
        const auto & location = call.region.descriptor.value();
        const auto & descriptor =
            location.prelude ? graph.prelude_context_events().at(location.observation) : graph.runtime_observations().at(location.observation);
        if (!descriptor.has_arg("layer_id")) continue;
        attention_by_layer[{ call.position.phase_index, call.position.decode_iteration, descriptor.arg_u64("layer_id") }].push_back(i);
    }

    for (auto & region : observe_dense_regions(source, queues, result.issues)) {
        const auto & descriptor = graph.runtime_observations().at(region.descriptor_observation.value_or(region.work.observation));
        if (descriptor.arg("model_layer_implementation") != "sglang.srt.models.qwen3.Qwen3DecoderLayer") continue;
        const bool embedding_output_mask = std::holds_alternative<structure::EmbeddingOutputMaskShape>(region.shape);
        const auto role = embedding_output_mask ? "input_embedding_output_mask" : descriptor.arg("dense_role");
        const bool gate = std::holds_alternative<structure::GatedActivationShape>(region.shape);
        const bool norm = std::holds_alternative<structure::NormalizationShape>(region.shape);
        const bool qkv = std::holds_alternative<structure::QkvRopeShape>(region.shape);
        const bool table = std::holds_alternative<structure::RotaryTableShape>(region.shape);
        const bool rotary = std::holds_alternative<structure::RotaryShape>(region.shape);
        const bool reshape = std::holds_alternative<structure::QkReshapeShape>(region.shape);
        const bool embedding = std::holds_alternative<structure::EmbeddingShape>(region.shape);
        const bool embedding_mask = std::holds_alternative<structure::EmbeddingMaskShape>(region.shape);
        const bool logits_output = std::holds_alternative<structure::LogitsOutputShape>(region.shape);
        const bool logits_pruning = std::holds_alternative<structure::LogitsPruningShape>(region.shape);
        const bool input_buffers = std::holds_alternative<structure::InputBuffersShape>(region.shape);
        const bool sampling = std::holds_alternative<structure::GreedySamplingShape>(region.shape);
        if (gate && role != "mlp_activation") continue;
        if (norm && role != "input_norm" && role != "post_attention_norm" && role != "query_norm" && role != "key_norm" && role != "output_norm") continue;
        if (role == "output_norm" && (!norm || descriptor.arg("model_implementation") != "sglang.srt.models.qwen3.Qwen3Model")) continue;
        if (role == "logits_projection" && (!region.implementation.starts_with("torch.matmul.logits:") || descriptor.arg("last_token_projection") != "true"))
            continue;
        if (qkv && role != "qkv_norm_rope") continue;
        if (table && role != "rotary_table") continue;
        if (rotary && role != "rotary") continue;
        if (reshape && role != "query_reshape" && role != "key_reshape") continue;
        if (embedding && role != "input_embedding") continue;
        if (embedding_mask && role != "input_embedding_mask") continue;
        if (logits_output && role != "logits_output") continue;
        if (logits_pruning && role != "logits_pruning") continue;
        if (input_buffers && role != "input_buffers") continue;
        if (sampling && role != "greedy_sampling") continue;
        if (!gate && !norm && !qkv && !table && !rotary && !reshape && !embedding && !embedding_mask && !embedding_output_mask && !logits_output
            && !logits_pruning && !input_buffers && !sampling && role != "qkv_projection" && role != "attention_output" && role != "mlp_gate_up"
            && role != "mlp_down" && role != "logits_projection")
            continue;
        const auto owner = call_phase_position(region.work, positions);
        if (!owner || !descriptor.has_arg("model_layer_index")) {
            ++result.issues["dense_call_phase_or_layer_unavailable"];
            continue;
        }

        const auto layer = descriptor.arg_u64("model_layer_index");
        if (table && (!owner->decode_iteration || !descriptor.has_arg("model_start_layer_index") || descriptor.arg_u64("model_start_layer_index") != layer)) {
            ++result.issues["rotary_table_shared_owner_unresolved"];
            continue;
        }
        const bool head_norm = role == "query_norm" || role == "key_norm";
        const auto rows_per_token = descriptor.arg_u64("rows_per_token");
        if (rows_per_token == 0 || (!head_norm && rows_per_token != 1)) {
            ++result.issues["dense_token_row_mapping_unavailable"];
            continue;
        }
        const auto match = attention_by_layer.find({ owner->phase_index, owner->decode_iteration, layer });
        if (match == attention_by_layer.end() || match->second.size() != 1) {
            ++result.issues["dense_call_token_layout_unresolved"];
            continue;
        }
        result.calls.push_back({ std::move(region), *owner, match->second.front(), role, layer, rows_per_token });
    }
    std::map<std::pair<Identity, std::string>, size_t> counts;
    std::set<size_t> sampling_descriptors;
    for (const auto & call : result.calls) {
        const auto count = ++counts[{
            { call.position.phase_index, call.position.decode_iteration, call.layer_index },
            call.role
        }];
        if (call.role == "greedy_sampling") {
            sampling_descriptors.insert(call.region.descriptor_observation.value());
            if (count > 1) ++result.issues["greedy_sampling_correspondence_ambiguous"];
        }
    }
    // A declared greedy call that cannot identify its inner argmax is a
    // coverage gap, not permission to treat unknown work as cache-independent.
    // Do not require a sampler in phases that never declared one (e.g. scoring).
    for (size_t i = 0; i < graph.runtime_observations().size(); ++i) {
        const auto & event = graph.runtime_observations()[i];
        if (event.name == "runtime.phase.greedy_sampling" && event.arg("model_layer_implementation") == "sglang.srt.models.qwen3.Qwen3DecoderLayer"
            && event.arg("formal_window_context") != "causal_head" && !sampling_descriptors.contains(i))
            ++result.issues["greedy_sampling_without_complete_call"];
    }
    for (const auto & [identity, calls] : attention_by_layer) {
        if (!qwen3_layers.contains(std::get<2>(identity))) continue;
        if (logits_layers.contains(std::get<2>(identity))) {
            for (const auto * role : { "logits_projection", "logits_output" }) {
                const auto count = counts[{ identity, role }];
                if (count == 0) ++result.issues[std::string(role) + "_without_complete_call"];
                else if (count > 1) ++result.issues[std::string(role) + "_correspondence_ambiguous"];
            }
        }
        if (const auto embedding = input_embedding_layers.find(std::get<2>(identity)); embedding != input_embedding_layers.end()) {
            for (const auto * role : { "input_embedding", "input_embedding_mask", "input_embedding_output_mask" }) {
                if (embedding->second == 1 && std::string_view(role) != "input_embedding") continue;

                const auto count = counts[{ identity, role }];
                if (count == 0) ++result.issues[std::string(role) + "_without_complete_call"];
                else if (count > 1) ++result.issues[std::string(role) + "_correspondence_ambiguous"];
            }
        }
        if (output_norm_layers.contains(std::get<2>(identity))) {
            const auto count = counts[{ identity, "output_norm" }];
            if (count == 0) ++result.issues["output_norm_without_complete_call"];
            else if (count > 1) ++result.issues["output_norm_correspondence_ambiguous"];
        }
        if (!std::get<1>(identity)) {
            for (const auto * role : { "rotary", "query_reshape", "key_reshape" }) {
                const auto count = counts[{ identity, role }];
                if (count == 0) ++result.issues[std::string(role) + "_without_complete_call"];
                else if (count > 1) ++result.issues[std::string(role) + "_correspondence_ambiguous"];
            }
        }
        if (std::get<1>(identity) && start_layers.contains(std::get<2>(identity))) {
            const auto count = counts[{ identity, "rotary_table" }];
            if (count == 0) ++result.issues["rotary_table_without_complete_call"];
            else if (count > 1) ++result.issues["rotary_table_correspondence_ambiguous"];
        }
        for (const auto * role : { "qkv_projection",
                                   "attention_output",
                                   "mlp_gate_up",
                                   "mlp_down",
                                   "mlp_activation",
                                   "input_norm",
                                   "post_attention_norm",
                                   "query_norm",
                                   "key_norm",
                                   "qkv_norm_rope" }) {
            // NPU Decode fuses Q/K normalization with split and rotary
            // encoding; it must not require or generate separate RMS calls.
            if (std::get<1>(identity) && (std::string_view(role) == "query_norm" || std::string_view(role) == "key_norm")) continue;
            if (!std::get<1>(identity) && std::string_view(role) == "qkv_norm_rope") continue;
            const auto count = counts[{ identity, role }];
            if (count == 0) ++result.issues["dense_operation_without_complete_call"];
            else if (count > 1) ++result.issues["dense_operation_correspondence_ambiguous"];
        }
    }
    return result;
}

AttentionMetadataAudit observe_attention_metadata_calls(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                        const simulation::detail::CpuTaskQueues & queues,
                                                        const modules::hicache::HiCachePhaseObservationAudit & phases) {
    AttentionMetadataAudit result;
    const auto positions = phase_call_positions(phases);
    using Round = std::pair<size_t, std::optional<uint64_t>>;
    std::map<Round, size_t> call_counts;
    for (const auto & [node, position] : positions.nodes)
        if (position) call_counts.try_emplace({ position->phase_index, position->decode_iteration }, 0);
    for (auto & region : observe_attention_metadata_regions(source, queues)) {
        const auto & descriptor = source.graph().runtime_observations().at(region.work.observation);
        // Capture is outside application replay; do not demand a target
        // execution replacement for its initialization-only operations.
        if (descriptor.arg("in_capture") == "true") continue;
        const auto owner = call_phase_position(region.work, positions);
        if (!owner) {
            ++result.issues["metadata_phase_ownership_unresolved"];
            continue;
        }
        const auto inputs = observed_attention_metadata_inputs(descriptor);
        if (!inputs || region.work.caller.status != "ready" || region.work.caller.has_node_overlap) {
            ++result.issues["metadata_inputs_or_boundary_unavailable"];
            continue;
        }

        const auto & phase = phases.observations.at(owner->phase_index);
        ++call_counts[{ owner->phase_index, owner->decode_iteration }];
        result.calls.push_back({ std::move(region), *owner, *inputs, inputs->context_tokens == phase.prompt_token_count });
    }
    // One ordinary metadata preparation per forward round. Missing probe
    // coverage cannot leave unclaimed metadata kernels in generic phase cost;
    // repeated/chunked rounds need their own explicit semantic identities.
    for (const auto & [round, count] : call_counts) {
        if (count == 0) ++result.issues["metadata_round_without_complete_call"];
        else if (count > 1) ++result.issues["metadata_round_has_multiple_calls"];
    }
    if (result.calls.empty()) ++result.issues["no_complete_metadata_calls"];
    return result;
}

AttentionCallAudit observe_attention_calls(const modules::hicache::patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                           const modules::hicache::HiCachePhaseObservationAudit & phases) {
    AttentionCallAudit result;
    if (!phases.ready()) {
        ++result.issues["source_phase_observations_incomplete"];
        return result;
    }

    const auto owners = phase_call_positions(phases);
    std::set<size_t> required_attention_nodes;
    const auto & graph = source.graph();
    for (size_t i = 0; i < phases.observations.size(); ++i) {
        const auto & phase = phases.observations[i];
        required_attention_nodes.insert(phase.prefill_prefix_attention_node_ids.begin(), phase.prefill_prefix_attention_node_ids.end());
        for (const auto & [node, iteration] : phase.decode_node_iterations) {
            if (!graph.node(node).is_cpu && modules::hicache::is_hicache_paged_attention(graph.event_for_node(node).name))
                required_attention_nodes.insert(node);
        }
    }
    const auto contexts = decode_operator_contexts(phases, observe_decode_contexts(graph, phases));
    std::set<size_t> covered_nodes;
    for (auto & region : observe_attention_regions(source, queues)) {
        if (region.status != "returned" || region.sampling != "every_call" || !region.descriptor) {
            ++result.issues["attention_call_observation_incomplete"];
            continue;
        }
        if (region.work.caller.status != "ready" || region.work.caller.has_node_overlap) {
            ++result.issues["attention_call_boundary_incomplete"];
            ++result.issues["attention_call_boundary: " + region.work.caller.reason];
            continue;
        }

        const auto owner = call_phase_position(region.work, owners);
        if (!owner || (region.phase == "decode") != owner->decode_iteration.has_value()) {
            ++result.issues["attention_call_phase_ownership_unresolved"];
            continue;
        }

        const auto & descriptor = region.descriptor->prelude ? graph.prelude_context_events().at(region.descriptor->observation)
                                                             : graph.runtime_observations().at(region.descriptor->observation);
        const auto conditions = attention_conditions(descriptor);
        const auto context = attention_decode_context(region, contexts);
        const auto batch = attention_batch_shape(descriptor, context ? std::optional<uint64_t>{ context->context_tokens } : std::nullopt);
        const auto write = attention_cache_write(descriptor);
        if (!conditions || !batch || !write) {
            ++result.issues["attention_call_input_metadata_missing"];
            continue;
        }

        covered_nodes.insert(region.work.submitted_nodes.begin(), region.work.submitted_nodes.end());
        result.calls.push_back(
            { std::move(region), *owner, *conditions, *batch, *write, attention_eager_padding(descriptor) });
    }
    for (const auto node : required_attention_nodes)
        if (!covered_nodes.contains(node)) ++result.issues["attention_device_work_without_call"];
    if (result.calls.empty()) ++result.issues["no_complete_attention_calls"];
    resolve_attention_cache_layouts(source, queues, phases, result);
    return result;
}

} // namespace markov::trace_graph::source_dag
