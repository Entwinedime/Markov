#include "markov/trace_graph/adapters/runtimes/ascend/operator_bindings.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace markov::trace_graph::adapters::ascend {

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

std::map<size_t, size_t> match_tensor_operators(const source_dag::SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
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

std::map<size_t, size_t> match_submitted_operator_envelopes(const source_dag::SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                                            std::span<const size_t> observations, const std::map<size_t, std::string> & expected) {
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
        const auto region = source_dag::observe_submitted_call_region(source, queues, observation);
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
    return source_dag::match_operator_envelopes(graph, submitted, expected);
}

std::optional<SynchronousReadback> observe_synchronous_readback(const source_dag::SourceDagIndex & source, const source_dag::SubmittedCallRegion & region) {
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

} // namespace markov::trace_graph::adapters::ascend
