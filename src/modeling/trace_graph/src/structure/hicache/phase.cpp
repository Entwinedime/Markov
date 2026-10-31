#include "markov/trace_graph/structure/hicache/phase.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/attention_program.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/dense_program.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/source_calls.hpp"

#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include "markov/trace_graph/structure/missing_work.hpp"
#include "markov/trace_graph/structure/phase_shape.hpp"
#include <array>
#include <span>
#include <string_view>
#include <utility>

namespace markov::trace_graph::structure::hicache {

std::string_view collective_task_name(CollectiveTaskKind kind) {
    switch (kind) {
    case CollectiveTaskKind::Record:
        return "record_event";
    case CollectiveTaskKind::Wait:
        return "wait_event";
    case CollectiveTaskKind::Reduce:
        return "HcclAllreduce";
    case CollectiveTaskKind::Gather:
        return "HcclAllGather";
    case CollectiveTaskKind::Destroy:
        return "destroy_event";
    }
    std::unreachable();
}

OperationWork collective_task_work(const OperationWork & collective, CollectiveTaskKind kind) {
    auto work = collective;
    const auto name = std::string(collective_task_name(kind));
    work.operation += "." + name;
    work.timing_scope = "worker_service";
    if (kind != CollectiveTaskKind::Reduce && kind != CollectiveTaskKind::Gather) {
        work.implementation = "torch_npu.queue:" + name;
        work.features = {
            { "event_count", 1.0 }
        };
    }
    // Reduction submission retains the collective's dtype specialization;
    // equal element widths do not make different HCCL data types identical.
    return work;
}

std::span<const CollectiveCallerStep> ordinary_collective_caller_steps(CollectiveTaskKind transfer) {
    using Kind = CollectiveTaskKind;
    static const std::array<CollectiveCallerStep, 8> steps{
        {
         { "prepare_input_record", Kind::Record },
         { "submit_input_wait", Kind::Wait },
         { "prepare_reduction", Kind::Reduce },
         { "publish_future_record", Kind::Record },
         { "publish_completion_record", Kind::Record },
         { "submit_output_wait", Kind::Wait },
         { "release_future_event", Kind::Destroy },
         { "return", std::nullopt },
         }
    };
    static const auto gather_steps = [] {
        auto result = steps;
        result[2] = { "prepare_gather", Kind::Gather };
        return result;
    }();
    return transfer == Kind::Gather ? std::span<const CollectiveCallerStep>{ gather_steps } : std::span<const CollectiveCallerStep>{ steps };
}

OperationWork collective_caller_work(const OperationWork & collective, const CollectiveCallerStep & step) {
    auto work = collective;
    work.operation += ".host." + std::string(step.name);
    work.timing_scope = "cpu_service";
    work.features = {
        { "input_tensor_count",                                    1.0 },
        {         "input_rank",                                    2.0 },
        {       "participants", collective.features.at("participants") }
    };
    return work;
}

namespace {

PhaseCallPlan plan_attention_call(const adapters::sglang::AttentionCall & source, const modules::hicache::model::HiCachePrefillWorkItem & prefill,
                                  const modules::hicache::model::HiCacheDecodeWorkItem & decode) {
    PhaseCallPlan result{ .source_observation = source.region.work.observation,
                          .logical_input = prefill.logical_input,
                          .request_id = prefill.request_id,
                          .decode_iteration = source.position.decode_iteration,
                          .layer_index = source.conditions.cache_layer_index };
    if (prefill.batch_size != 1 || source.batch.requests.size() != 1) {
        result.reason = "target_attention_batch_rule_not_implemented";
        return result;
    }
    const adapters::sglang::AttentionRequestShape request =
        source.position.decode_iteration
            ? adapters::sglang::AttentionRequestShape{ 1, decode_context_tokens(decode.prompt_token_count, *source.position.decode_iteration) }
            : adapters::sglang::AttentionRequestShape{ prefill.prefill_token_count, prefill.prompt_token_count };
    const auto page_tokens = source.position.decode_iteration ? decode.target_page_size : prefill.target_page_size;
    const auto & original = source.batch.requests.front();
    if (request.query_tokens == original.query_tokens && request.context_tokens == original.context_tokens && page_tokens == source.batch.page_tokens) {
        result.action = PhaseCallPlan::Action::RetainCompute;
        result.reason = "same_compute_geometry_hicache_waits_remain_independently_generated";
        return result;
    }

    std::optional<adapters::sglang::AttentionInputs> inputs;
    if (source.position.decode_iteration && source.conditions.graph && request.context_tokens == original.context_tokens) {
        // Only the cache page geometry changed. The same request/round retains
        // graph eligibility and captured query batch; do not infer a new graph
        // bucket for changed batching or context lengths from a source sample.
        inputs = adapters::sglang::AttentionInputs{ source.batch, source.cache_write };
        inputs->batch.page_tokens = page_tokens;
    }
    else if (source.padding) {
        if (source.conditions.prefill && source.padding->allow_input_scattered) {
            result.reason = "target_tbo_choice_not_established";
            return result;
        }
        inputs = adapters::sglang::target_eager_attention_inputs(source.conditions,
                                                                 source.batch,
                                                                 source.cache_write,
                                                                 *source.padding,
                                                                 std::span<const adapters::sglang::AttentionRequestShape>{ &request, 1 },
                                                                 page_tokens);
    }
    if (!inputs) {
        result.reason = "target_attention_layout_rule_or_metadata_missing";
        return result;
    }
    auto operations = adapters::sglang::ascend_attention_tensor_program(source.conditions, inputs->batch, inputs->cache_write);
    if (!operations) {
        result.reason = "target_attention_program_rule_not_implemented";
        return result;
    }
    result.action = PhaseCallPlan::Action::Generate;
    result.operations = std::move(*operations);
    return result;
}

PhaseCallPlan plan_attention_metadata_call(const adapters::sglang::AttentionMetadataCall & source,
                                           const modules::hicache::model::HiCachePrefillWorkItem & prefill,
                                           const modules::hicache::model::HiCacheDecodeWorkItem & decode) {
    PhaseCallPlan result{ .source_observation = source.region.work.observation,
                          .logical_input = prefill.logical_input,
                          .request_id = prefill.request_id,
                          .decode_iteration = source.position.decode_iteration,
                          .call_kind = "metadata" };
    if (prefill.batch_size != 1 || (!source.position.decode_iteration && !source.complete_prefill_context)) {
        result.reason = "target_metadata_batch_or_prefill_chunk_rule_unavailable";
        return result;
    }

    auto target = source.inputs;
    target.context_tokens =
        source.position.decode_iteration ? decode_context_tokens(decode.prompt_token_count, *source.position.decode_iteration) : prefill.prompt_token_count;
    target.page_tokens = source.position.decode_iteration ? decode.target_page_size : prefill.target_page_size;
    if (target.context_tokens > target.max_context_tokens) {
        result.reason = "target_metadata_context_exceeds_deployment_capacity";
        return result;
    }
    if (target.context_tokens == source.inputs.context_tokens && target.page_tokens == source.inputs.page_tokens) {
        result.action = PhaseCallPlan::Action::RetainCompute;
        result.reason = "same_metadata_context_page_geometry_and_execution_path";
        return result;
    }
    // A cache-page change does not change this single-request graph bucket.
    // Changed context/dispatch needs a separately established graph decision.
    if (target.graph && target.context_tokens != source.inputs.context_tokens) {
        result.reason = "target_metadata_graph_dispatch_not_established";
        return result;
    }
    auto program = adapters::sglang::ascend_attention_metadata_program(source.position.decode_iteration ? "decode" : "prefill", target);
    if (!program) {
        result.reason = "target_metadata_backend_lowering_unavailable";
        return result;
    }
    result.action = PhaseCallPlan::Action::Generate;
    result.operations = std::move(*program);
    return result;
}

PhaseCallPlan plan_dense_call(const adapters::sglang::DenseCall & source, const adapters::sglang::AttentionCall & attention,
                              const modules::hicache::model::HiCachePrefillWorkItem & prefill) {
    PhaseCallPlan result{ .source_observation = source.region.work.observation,
                          .logical_input = prefill.logical_input,
                          .request_id = prefill.request_id,
                          .decode_iteration = source.position.decode_iteration,
                          .layer_index = source.layer_index,
                          .call_kind = "dense." + source.role };
    if (prefill.batch_size != 1 || attention.batch.requests.size() != 1 || !attention.padding || attention.padding->mlp_sync
        || attention.padding->allow_input_scattered || attention.conditions.context_parallel) {
        result.reason = "target_dense_distributed_token_layout_unavailable";
        return result;
    }
    // Qwen3 projections/gating preserve token rows. Q/K RMSNorm flattens
    // token and head axes, with the local head count declared by the module.
    // This equality corroborates that rule in the source; it is not how an
    // arbitrary matrix dimension is classified as token-major.
    // Ordinary single-request logits without input logprobs have already
    // selected the last token. The source descriptor proves that branch;
    // a changed Prefill length does not change the projection's row count.
    const bool logits = source.role == "logits_projection" || source.role == "logits_output" || source.role == "greedy_sampling";
    const auto source_rows = logits ? 1 : core::checked_multiply_u64(attention.batch.padded_query_tokens, source.rows_per_token, "Dense source rows overflow");
    if (dense_rows(source.region.shape) != source_rows) {
        result.reason = "source_dense_rows_disagree_with_declared_dense_token_layout";
        return result;
    }
    auto target_tokens = source.position.decode_iteration ? attention.batch.padded_query_tokens : prefill.prefill_token_count;
    if (logits) target_tokens = 1;
    const auto rows = core::checked_multiply_u64(target_tokens, source.rows_per_token, "Dense target rows overflow");
    if (rows == 0) {
        result.reason = "target_empty_dense_branch_not_generated";
        return result;
    }
    if (dense_rows(source.region.shape) == rows) {
        result.action = PhaseCallPlan::Action::RetainCompute;
        result.reason = "same_dense_operation_shape_dtype_parameters_and_layout";
        return result;
    }
    if (attention.conditions.graph) {
        result.reason = "target_dense_graph_layout_change_not_established";
        return result;
    }
    const auto shape = target_dense_shape(source.region.shape, rows);
    if (!shape) {
        result.reason = std::holds_alternative<RotaryShape>(source.region.shape) ? "target_rotary_selects_unmodeled_fused_input_layout"
                                                                                 : "target_dense_input_layout_rule_unavailable";
        return result;
    }
    result.action = PhaseCallPlan::Action::Generate;
    result.operations = adapters::sglang::dense_tensor_program(source.position.decode_iteration ? "decode" : "prefill", source.region.implementation, *shape);
    return result;
}

DeviceCollectivePlan plan_device_collective_call(const adapters::sglang::DeviceCollectiveCall & source,
                                                 const adapters::sglang::AttentionCallAudit & attention_calls,
                                                 const modules::hicache::model::HiCachePrefillWorkItem & prefill) {
    DeviceCollectivePlan result{ source.region.observation, source.peer_observations, std::nullopt, {} };
    const adapters::sglang::AttentionCall * attention = nullptr;
    if (const auto at = attention_calls.cache_layer_calls.find({ source.position.phase_index, source.position.decode_iteration, source.layer_index });
        at != attention_calls.cache_layer_calls.end()) {
        if (!at->second) {
            result.reason = "collective_forward_layout_ambiguous";
            return result;
        }
        attention = &attention_calls.calls.at(*at->second);
    }
    if (!attention || !attention->padding || prefill.batch_size != 1 || attention->batch.requests.size() != 1 || attention->padding->mlp_sync
        || attention->padding->allow_input_scattered || attention->conditions.context_parallel) {
        result.reason = "collective_distributed_token_layout_unavailable";
        return result;
    }
    const bool mlp_output = source.role == "mlp_down" && source.callsite == "forward";
    const bool attention_output = source.role == "communication" && source.callsite == "prepare_mlp";
    const bool embedding_output = source.role == "input_embedding" && source.callsite == "forward";
    const bool logits_output = source.transfer == CollectiveTaskKind::Gather && source.role == "logits_projection" && source.callsite == "_get_logits";
    if (!mlp_output && !attention_output && !embedding_output && !logits_output) {
        result.reason = "collective_callsite_dispatch_rule_unavailable";
        return result;
    }
    if (source.rows != (logits_output ? 1 : attention->batch.padded_query_tokens)) {
        result.reason = "collective_source_rows_disagree_with_token_layout";
        return result;
    }

    // In the supported unscattered Qwen3 path, these reductions operate on
    // [local token rows, hidden width]. Page size/context do not change this
    // payload. Decode retains its declared captured/eager padded query rows.
    const auto rows = logits_output ? 1 : source.position.decode_iteration ? attention->batch.padded_query_tokens : prefill.prefill_token_count;
    if (!rows || (attention->conditions.graph && rows != source.rows)) {
        result.reason = "collective_empty_or_changed_graph_dispatch_unavailable";
        return result;
    }
    const std::string component = source.position.decode_iteration ? "decode" : "prefill";
    const CollectiveShape shape{ rows, source.width, source.element_bytes, source.members.size() };
    const std::string operation = logits_output ? "all_gather" : "all_reduce";
    result.service = OperationWork{ component,
                                    component + "." + operation,
                                    "sglang.NpuCommunicator." + operation + ":" + source.dtype,
                                    "collective_completion_interval",
                                    shape.features() };
    return result;
}

} // namespace

PhaseCallWork plan_phase_calls(const adapters::sglang::PhaseCallAudit & source, size_t source_phase,
                               const modules::hicache::model::HiCachePrefillWorkItem & prefill, const modules::hicache::model::HiCacheDecodeWorkItem & decode) {
    PhaseCallWork result;
    for (const auto & call : source.attention.calls)
        if (call.position.phase_index == source_phase) result.tensors.push_back(plan_attention_call(call, prefill, decode));

    for (const auto & call : source.metadata.calls)
        if (call.position.phase_index == source_phase) result.tensors.push_back(plan_attention_metadata_call(call, prefill, decode));

    for (const auto & call : source.dense.calls) {
        if (call.position.phase_index != source_phase) continue;

        result.tensors.push_back(plan_dense_call(call, source.attention.calls.at(call.attention_call_index), prefill));
    }

    for (const auto & call : source.collectives.calls)
        if (call.position.phase_index == source_phase) result.collectives.push_back(plan_device_collective_call(call, source.attention, prefill));

    return result;
}

std::map<size_t, CostMigration> phase_migration_candidates(const core::DagGraph & graph, const modules::hicache::HiCachePhaseObservation & source,
                                                           const modules::hicache::model::HiCachePrefillWorkItem & prefill,
                                                           const modules::hicache::model::HiCacheDecodeWorkItem & decode) {
    std::map<size_t, CostMigration> operations;
    const auto append = [&](const auto & nodes, const std::string & operation, bool unchanged) {
        for (const auto id : nodes) { operations.emplace(id, CostMigration{ operation, graph.event_for_node(id).name, unchanged }); }
    };

    // Same model/TP/backend is established by the prediction input boundary.
    // A token match alone does not prove identical operator work.
    const bool same_prefill = prefill.prefill_token_count == source.prefill_token_count
                              && prefill.reusable_prefix_token_count == source.prompt_token_count - source.prefill_token_count
                              && prefill.batch_size == source.batch_size && prefill.target_page_size == source.source_page_size;
    append(prefill.common_kernel_nodes, "prefill.common", same_prefill);
    append(prefill.prefix_attention_nodes, "prefill.attention", same_prefill);
    append(prefill.collective_nodes, "prefill.collective", same_prefill);
    append(prefill.submit_nodes, "prefill.submit", same_prefill);

    const bool same_context = decode.prompt_token_count == source.prompt_token_count && prefill.batch_size == source.batch_size;
    const bool same_pages = decode.target_page_size == source.source_page_size;
    for (const auto id : decode.kernel_nodes) {
        const bool attention = adapters::sglang::is_attention_kernel(graph.event_for_node(id).name);
        // Non-attention names do not establish cache independence: forward
        // metadata also submits indexing, casts and page-table division here.
        // Until semantic ownership proves otherwise, require the same cache
        // geometry for migration just as for the rest of this phase.
        append(std::vector<size_t>{ id }, attention ? "decode.attention" : "decode.common", same_context && same_pages);
    }
    // These operators retain their own corresponding round, not another
    // round's average. Cache pages are not the kernel's internal tile size.
    append(decode.collective_nodes, "decode.collective", same_context);
    append(decode.submit_nodes, "decode.submit", same_context && same_pages);
    return operations;
}

void require_complete_phase_migrations(const std::map<size_t, CostMigration> & operations) {
    std::map<std::string, size_t> unresolved;
    for (const auto & [node, binding] : operations)
        if (!binding.unchanged) ++unresolved[binding.operation + ":" + binding.implementation];

    if (!unresolved.empty()) throw MissingWork("Changed phase operations lack structural work rules", unresolved);
}

} // namespace markov::trace_graph::structure::hicache
