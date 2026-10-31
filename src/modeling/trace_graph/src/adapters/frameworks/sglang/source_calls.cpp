#include "markov/trace_graph/adapters/frameworks/sglang/source_calls.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/allocator_program.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/attention_program.hpp"
#include "markov/trace_graph/adapters/runtimes/ascend/operator_bindings.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/source_dag/operator_envelopes.hpp"

#include <algorithm>
#include <initializer_list>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <tuple>
#include <utility>

namespace markov::trace_graph::adapters::sglang {

std::vector<AllocatorRegion> observe_allocator_regions(const source_dag::SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues) {
    const auto & graph = source.graph();
    std::vector<AllocatorRegion> regions;
    const auto & observations = graph.runtime_observations();
    const auto & operators = source.operator_envelopes();
    for (size_t i = 0; i < observations.size(); ++i) {
        const auto & event = observations[i];
        if (event.name != "runtime.hicache.extend_allocator" && event.name != "runtime.hicache.decode_allocator") continue;
        if (event.arg("status") != "returned") continue;

        std::optional<bool> allocated;
        if (event.arg("allocated") == "true") allocated = true;
        else if (event.arg("allocated") == "false") allocated = false;

        regions.push_back({
            .work = source_dag::observe_submitted_call_region(source, queues, i),
            .phase = event.name == "runtime.hicache.extend_allocator" ? modules::hicache::model::HiCacheAllocatorPhase::Extend
                                                                      : modules::hicache::model::HiCacheAllocatorPhase::Decode,
            .allocated = allocated,
            .operator_observations = operators.inside(event),
        });
    }
    return regions;
}

std::optional<adapters::sglang::AscendAllocatorInputs> observed_allocator_inputs(const core::TraceEvent & event) {
    if (event.arg("allocator_implementation") != "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator"
        || !event.has_arg_key_hint("sequence_dtype") || !event.has_arg_key_hint("last_location_dtype") || !event.has_arg_key_hint("free_page_dtype")
        || (event.arg("debug_memory_pool") != "true" && event.arg("debug_memory_pool") != "false"))
        return std::nullopt;

    adapters::sglang::AscendAllocatorInputs inputs{
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

std::map<std::string, adapters::sglang::AscendAllocatorInputs> observe_allocator_inputs(const core::DagGraph & graph) {
    std::map<std::string, adapters::sglang::AscendAllocatorInputs> result;
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

std::vector<AttentionRegion> observe_attention_regions(const source_dag::SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues) {
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
    const auto & operators = source.operator_envelopes();
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
        regions.push_back(
            { source_dag::observe_submitted_call_region(source, queues, i), event.arg("phase"), event.arg("sampling"), event.arg("status"), descriptor });
        regions.back().operator_observations = operators.inside(event);
    }
    return regions;
}

std::vector<AttentionMetadataRegion> observe_attention_metadata_regions(const source_dag::SourceDagIndex & source,
                                                                        const simulation::detail::CpuTaskQueues & queues) {
    const auto & events = source.graph().runtime_observations();
    const auto & operators = source.operator_envelopes();
    std::vector<AttentionMetadataRegion> regions;
    for (size_t i = 0; i < events.size(); ++i) {
        const auto & event = events[i];
        if (event.source_channel != core::TraceSourceChannel::PythonProbe || event.name != "runtime.phase.attention_metadata") continue;
        if (event.arg("formal_window_context") == "causal_head") continue;

        regions.push_back({ source_dag::observe_submitted_call_region(source, queues, i), operators.inside(event) });
    }
    return regions;
}


namespace {

std::optional<adapters::sglang::AscendAttentionConditions> attention_conditions(const nlohmann::json & fields) {
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
    return adapters::sglang::AscendAttentionConditions{
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

std::optional<adapters::sglang::AttentionBatchShape> attention_batch_shape(const nlohmann::json & fields, std::optional<uint64_t> observed_decode_context) {
    for (const auto key : { "phase", "query_shape", "batch_size", "query_heads", "key_heads", "page_size_tokens" })
        if (!fields.contains(key) || fields.at(key).is_null()) return std::nullopt;

    const auto dimensions = fields.at("query_shape").get<std::vector<int64_t>>();
    const auto batch_size = fields.at("batch_size").get<size_t>();
    // This is an external trace boundary. Incomplete/truncated lists cannot
    // establish a different (smaller) batch or an unsigned wrapped token count.
    if (dimensions.empty() || dimensions.front() < 0) return std::nullopt;

    adapters::sglang::AttentionBatchShape result{
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

std::optional<source_dag::DecodeOperationContext> attention_decode_context(const AttentionRegion & region,
                                                                           const source_dag::DecodeOperatorContexts & contexts) {
    if (region.phase != "decode" || region.status != "returned") return std::nullopt;

    std::optional<source_dag::DecodeOperationContext> result;
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

std::optional<adapters::sglang::AttentionCacheWrite> attention_cache_write(const nlohmann::json & fields) {
    for (const auto key : { "save_kv_cache", "key_present", "value_present", "phase", "graph_mode", "enable_torch_compile" })
        if (!fields.contains(key)) return std::nullopt;
    if (!fields.at("save_kv_cache").get<bool>()) return adapters::sglang::AttentionCacheWrite{ .enabled = false };

    const bool graph_decode = fields.at("phase") == "decode" && fields.at("graph_mode").get<bool>() && !fields.at("enable_torch_compile").get<bool>();
    if (!graph_decode && (!fields.at("key_present").get<bool>() || !fields.at("value_present").get<bool>()))
        return adapters::sglang::AttentionCacheWrite{ .enabled = false };
    if (!fields.contains("cache_write")) return std::nullopt;

    const auto & write = fields.at("cache_write");
    for (const auto key :
         { "pool_implementation", "use_fia", "cache_dtype", "key_dtype", "value_dtype", "location_dtype", "key_elements", "value_elements", "locations" })
        if (!write.contains(key) || write.at(key).is_null()) return std::nullopt;

    return adapters::sglang::AttentionCacheWrite{
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

std::optional<adapters::sglang::AttentionEagerPadding> attention_eager_padding(const nlohmann::json & fields) {
    if (!fields.contains("padding")) return std::nullopt;
    const auto & padding = fields.at("padding");
    for (const auto key : { "mlp_sync", "allow_input_scattered", "tp_size", "can_run_tbo" })
        if (!padding.contains(key) || padding.at(key).is_null()) return std::nullopt;
    const auto tp_size = padding.at("tp_size").get<int64_t>();
    if (tp_size <= 0) return std::nullopt;

    return adapters::sglang::AttentionEagerPadding{ padding.at("mlp_sync").get<bool>(),
                                                    padding.at("allow_input_scattered").get<bool>(),
                                                    static_cast<uint64_t>(tp_size),
                                                    padding.at("can_run_tbo").get<bool>() };
}

} // namespace

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

std::optional<PhaseCallPosition> call_phase_position(const source_dag::SubmittedCallRegion & region, const PhaseCallPositions & positions) {
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

std::optional<adapters::sglang::AttentionMetadataInputs> observed_attention_metadata_inputs(const core::TraceEvent & descriptor) {
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

    adapters::sglang::AttentionMetadataInputs result{ route == "graph", context_tokens, page_tokens, max_context, descriptor.arg("device_name"),
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

AttentionMetadataAudit observe_attention_metadata_calls(const source_dag::SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                                        const modules::hicache::HiCachePhaseObservationAudit & phases, const PhaseCallPositions & positions) {
    AttentionMetadataAudit result;
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

AttentionCallAudit observe_attention_calls(const source_dag::SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                           const modules::hicache::HiCachePhaseObservationAudit & phases, const PhaseCallPositions & positions) {
    AttentionCallAudit result;
    if (!phases.ready()) {
        ++result.issues["source_phase_observations_incomplete"];
        return result;
    }

    std::set<size_t> required_attention_nodes;
    const auto & graph = source.graph();
    for (size_t i = 0; i < phases.observations.size(); ++i) {
        const auto & phase = phases.observations[i];
        required_attention_nodes.insert(phase.prefill_prefix_attention_node_ids.begin(), phase.prefill_prefix_attention_node_ids.end());
        for (const auto & [node, iteration] : phase.decode_node_iterations) {
            if (!graph.node(node).is_cpu && adapters::sglang::is_attention_kernel(graph.event_for_node(node).name)) required_attention_nodes.insert(node);
        }
    }
    const auto contexts = source_dag::decode_operator_contexts(phases, source_dag::observe_decode_contexts(graph, phases));
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

        const auto owner = call_phase_position(region.work, positions);
        if (!owner || (region.phase == "decode") != owner->decode_iteration.has_value()) {
            ++result.issues["attention_call_phase_ownership_unresolved"];
            continue;
        }

        const auto & descriptor = region.descriptor->prelude ? graph.prelude_context_events().at(region.descriptor->observation)
                                                             : graph.runtime_observations().at(region.descriptor->observation);
        // Parse the source descriptor once. Each semantic projection uses
        // these same recorded fields, never target timing or a guessed layout.
        const auto fields = nlohmann::json::parse(descriptor.args_json_view());
        const auto conditions = attention_conditions(fields);
        const auto context = attention_decode_context(region, contexts);
        const auto batch = attention_batch_shape(fields, context ? std::optional<uint64_t>{ context->context_tokens } : std::nullopt);
        const auto write = attention_cache_write(fields);
        if (!conditions || !batch || !write) {
            ++result.issues["attention_call_input_metadata_missing"];
            continue;
        }

        covered_nodes.insert(region.work.submitted_nodes.begin(), region.work.submitted_nodes.end());
        result.calls.push_back({ std::move(region), *owner, *conditions, *batch, *write, attention_eager_padding(fields) });
    }
    for (const auto node : required_attention_nodes)
        if (!covered_nodes.contains(node)) ++result.issues["attention_device_work_without_call"];
    if (result.calls.empty()) ++result.issues["no_complete_attention_calls"];
    resolve_attention_cache_layouts(source, queues, positions, result);
    for (size_t i = 0; i < result.calls.size(); ++i) {
        const auto & call = result.calls[i];
        if (!call.conditions.cache_layer_index) continue;

        const auto [at, inserted] =
            result.cache_layer_calls.try_emplace(std::tuple{ call.position.phase_index, call.position.decode_iteration, *call.conditions.cache_layer_index },
                                                 i);
        if (!inserted) at->second.reset();
    }
    return result;
}


std::map<std::string, size_t> PhaseCallAudit::issues() const {
    auto result = attention.issues;

    for (const auto * family : { &metadata.issues, &dense.issues, &collectives.issues })
        for (const auto & [issue, count] : *family) result[issue] += count;

    return result;
}

PhaseCallAudit observe_phase_calls(const source_dag::SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                   const modules::hicache::HiCachePhaseObservationAudit & phases) {
    PhaseCallAudit result;
    const auto positions = phase_call_positions(phases);

    result.attention = observe_attention_calls(source, queues, phases, positions);
    result.metadata = observe_attention_metadata_calls(source, queues, phases, positions);
    result.dense = observe_dense_calls(source, queues, positions, result.attention);
    result.collectives = observe_device_collectives(source, queues, phases, positions);

    return result;
}

} // namespace markov::trace_graph::adapters::sglang
