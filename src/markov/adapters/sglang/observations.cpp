#include "markov/adapters/sglang/observations.hpp"

#include "markov/adapters/sglang/allocation.hpp"
#include "markov/adapters/sglang/input_preparation.hpp"

#include "markov/adapters/sglang/dense.hpp"
#include "markov/adapters/sglang/metadata.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace markov::adapters::sglang {

using namespace trace_graph::source;
namespace {

using namespace trace_graph::planning;

std::vector<ComputeObservation> read_compute(const ProbeEvent & event, const ComputeFrame & frame, const AttentionDescriptor * descriptor) {
    if (event.name == "runtime.phase.attention") {
        if (!descriptor->operation || frame.sequence_tokens.size() != 1 || !frame.query_tokens || frame.query_tokens->size() != 1) return {};
        std::vector<ComputeObservation> result;
        for (const auto & write : descriptor->cache_writes) {
            const auto rows =
                descriptor->tensor_query_tokens == frame.query_tokens->front() ? std::optional(RowBinding{ RowBasis::QueryTokens, 1 }) : std::nullopt;
            result.push_back({ write, RowInput{ descriptor->tensor_query_tokens }, rows });
        }
        result.push_back({
            *descriptor->operation,
            FrameInput{ frame.query_tokens->front(), descriptor->tensor_query_tokens, frame.sequence_tokens.front(), 1, descriptor->page_tokens },
            std::nullopt
        });
        return result;
    }

    return read_dense_work(event);
}

std::optional<std::vector<ComputeOperation>> attention_writes(const nlohmann::json & args) {
    if (!args.at("save_kv_cache").get<bool>() || !args.at("key_present").get<bool>() || !args.at("value_present").get<bool>())
        return std::vector<ComputeOperation>{};

    const auto & cache = args.at("cache_write");
    if (cache.at("pool_implementation") != "sglang.srt.hardware_backend.npu.memory_pool_npu.NPUMHATokenToKVPool" || cache.at("use_fia") != true
        || cache.at("cache_dtype") != "torch.bfloat16" || cache.at("key_dtype") != cache.at("cache_dtype") || cache.at("value_dtype") != cache.at("cache_dtype")
        || cache.at("location_dtype") != "torch.int64")
        return std::nullopt;

    std::vector<ComputeOperation> result;
    for (const auto * tensor : { "key", "value" }) {
        const auto shape = cache.at(std::string(tensor) + "_shape").get<std::vector<double>>();
        const auto stride = cache.at(std::string(tensor) + "_stride").get<std::vector<double>>();
        if (shape.size() != 3 || stride.size() != 3 || stride[2] != 1 || stride[1] != shape[2] || shape[0] != args.at("query_shape").at(0).get<double>()
            || cache.at("locations").get<double>() != shape[0])
            return std::nullopt;

        result.push_back({
            "ascend.scatter_nd_update_",
            CacheWriteGeometry{ "attention." + std::string(tensor) + "_write",
                               shape[1] * shape[2],
                               stride[0],
                               "ascend.slice.bfloat16.aicore", "ascend.scatter_nd_update.bfloat16.aicore" }
        });
    }
    return result;
}

} // namespace

void ProbeReader::observe(const ProbeEvent & event) {
    const auto & name = event.name;
    const auto & args = event.args;
    const auto & process = event.process;
    if (name == "runtime.hicache.extend_allocator") {
        const auto frame = forward_frames_.find(process);
        if (frame == forward_frames_.end()) ++output_.counts["extend_allocator_without_forward_context"];
        else {
            const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + process + ":" + event.thread);
            output_.compute_frames[frame->second].preparations.push_back({
                { resource, event.begin_ns, event.end_ns, event.record },
                read_extend_allocator(args),
                "prefill_allocation"
            });
            input_windows_.insert_or_assign(process,
                                            InputWindow{
                                                { resource, event.begin_ns, event.end_ns, event.record },
                                                {},
                                                {}
            });
        }
    }
    if (name == "runtime.hicache.decode_allocator") {
        const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + process + ":" + event.thread);
        if (pending_decode_allocations_.contains(process)) ++output_.counts["unbound_decode_allocator"];
        pending_decode_allocations_.insert_or_assign(process,
                                                     ObservedCallSequence{
                                                         { resource, event.begin_ns, event.end_ns, event.record },
                                                         read_decode_allocator(args),
                                                         "decode_allocation"
        });
    }
    if (name == "runtime.hicache.decode_allocation") {
        ComputeFrame frame{ trace_graph::planning::Phase::Decode,
                            {},
                            args.at("sequence_lens_tokens").get<std::vector<double>>(),
                            {},
                            args.at("page_size_tokens").get<double>(),
                            event.record,
                            {} };
        for (const auto & identity : args.at("request_ids")) frame.requests.push_back(output_.name(identity.get<std::string>()));
        frame.query_tokens = std::vector<double>(frame.requests.size(), args.at("token_per_req").get<double>());
        const auto allocation = pending_decode_allocations_.find(process);
        if (allocation != pending_decode_allocations_.end() && event.begin_ns <= allocation->second.region.begin_ns
            && allocation->second.region.end_ns <= event.end_ns && args.at("token_per_req") == 1) {
            // The inner allocator record is emitted before its enclosing call.
            input_windows_.insert_or_assign(process, InputWindow{ allocation->second.region, {}, {} });
            frame.preparations.push_back(std::move(allocation->second));
            pending_decode_allocations_.erase(allocation);
        }
        else {
            const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + process + ":" + event.thread);
            frame.preparations.push_back({
                { resource, event.begin_ns, event.end_ns, event.record },
                nullptr,
                "decode_allocation"
            });
            ++output_.counts["decode_allocator_without_matching_call"];
        }
        forward_frames_[process] = output_.compute_frames.size();
        output_.compute_frames.push_back(std::move(frame));
    }

    if ((name == "runtime.hicache.extend_allocation" || name == "runtime.hicache.decode_allocation") && args.at("status") == "returned") {
        const auto window = input_windows_.find(process);
        if (window != input_windows_.end()) {
            const auto & allocator = window->second.allocator;
            window->second.table_write = ObservedRegion{ allocator.resource, allocator.end_ns, event.end_ns, event.record };
        }
    }
    if (name == "runtime.phase.input_buffers" && args.at("status") == "returned") {
        const auto window = input_windows_.find(process);
        if (window != input_windows_.end() && window->second.table_write) {
            const auto & table = *window->second.table_write;
            window->second.forward = ObservedRegion{ table.resource, table.end_ns, event.begin_ns, event.record };
        }
    }

    if (name == "hicache_cache_lookup_input_end" || name == "hicache_cache_finished_req_lifecycle_commit_end") {
        const auto path = token_path(args.at("token_dictionary"));
        const auto role = name == "hicache_cache_lookup_input_end" ? TokenRole::LookupInput : TokenRole::Committed;
        output_.observe_tokens(args.at("request_id").get<std::string>(), role, path, event.record);
    }
    if (name == "hicache_cache_extend_input_start") {
        const auto & dictionaries = args.at("token_dictionaries");
        const auto & identities = args.at("request_ids");
        ComputeFrame frame{ trace_graph::planning::Phase::Prefill, {}, {}, std::nullopt, std::nullopt, event.record, {} };
        for (std::size_t i = 0; i < identities.size(); ++i) {
            const auto path = token_path(dictionaries.at(i));
            const auto request = identities[i].get<std::string>();
            output_.observe_tokens(request, TokenRole::Prompt, path, event.record);
            frame.requests.push_back(output_.name(request));
            frame.sequence_tokens.push_back(output_.token_paths.length(path));
        }
        forward_frames_[process] = output_.compute_frames.size();
        output_.compute_frames.push_back(std::move(frame));
    }

    if (name == "runtime.phase.attention_metadata" && args.at("status") == "returned") {
        const auto frame = forward_frames_.find(process);
        if (frame == forward_frames_.end()) ++output_.counts["metadata_without_forward_context"];
        else {
            const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + process + ":" + event.thread);
            auto & preparations = output_.compute_frames[frame->second].preparations;
            const auto window = input_windows_.find(process);
            if (window != input_windows_.end() && window->second.table_write && window->second.forward) {
                preparations.push_back({ *window->second.table_write, read_request_table_write(args), "request_table_write" });
                preparations.push_back(
                    { *window->second.forward, deterministic_sampling_ ? read_forward_inputs(args, *deterministic_sampling_) : nullptr, "forward_inputs" });
                input_windows_.erase(window);
            }
            else ++output_.counts["input_preparation_boundaries_unavailable"];
            sequence_dtypes_[process] = args.at("sequence_dtype").get<std::string>();
            preparations.push_back({
                { resource, event.begin_ns, event.end_ns, event.record },
                read_metadata(args),
                "attention_metadata"
            });
        }
    }

    if (name == "runtime.phase.embedding" && args.at("status") == "returned") embedding_windows_[process] = { event.end_ns, 0 };
    if (name == "runtime.phase.device_collective" && args.at("dense_role") == "input_embedding" && args.at("status") == "returned")
        embedding_windows_.at(process).reduction_begin = event.begin_ns;
    if (name == "runtime.phase.embedding_forward" && args.at("status") == "returned" && args.at("tp_size").get<int>() > 1) {
        const auto window = embedding_windows_.at(process);
        if (window.lookup_end < event.begin_ns || window.reduction_begin <= window.lookup_end || window.reduction_begin > event.end_ns)
            throw std::runtime_error("Embedding mask has no bounded native lookup-to-reduction window");

        auto mask = event;
        mask.begin_ns = window.lookup_end;
        mask.end_ns = window.reduction_begin;
        observe_compute(mask);
    }

    constexpr std::array<std::string_view, 15> compute_roles{
        "runtime.phase.linear",           "runtime.phase.logits_projection", "runtime.phase.normalization",
        "runtime.phase.gated_activation", "runtime.phase.attention",         "runtime.phase.input_buffers",
        "runtime.phase.embedding_mask",   "runtime.phase.embedding",         "runtime.phase.qk_reshape",
        "runtime.phase.rotary",           "runtime.phase.rotary_table",      "runtime.phase.qkv_norm_rope",
        "runtime.phase.logits_output",    "runtime.phase.logits_pruning",    "runtime.phase.greedy_sampling"
    };
    if (name == "runtime.phase.logits_output" && args.at("status") == "returned") logits_output_ends_[process] = event.end_ns;
    if (name == "runtime.phase.greedy_sampling" && args.at("status") == "returned") {
        const auto frame = forward_frames_.find(process);
        if (frame != forward_frames_.end() && output_.compute_frames[frame->second].phase == trace_graph::planning::Phase::Prefill) {
            const auto before = logits_output_ends_.find(process);
            if (before != logits_output_ends_.end()) {
                const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + process + ":" + event.thread);
                output_.compute_frames[frame->second].preparations.push_back({
                    { resource, before->second, event.begin_ns, event.record },
                    sampling_positions(sequence_dtypes_.at(process)),
                    "sampling_positions"
                });
            }
            else ++output_.counts["sampling_position_boundary_unavailable"];
        }
    }
    if (std::ranges::find(compute_roles, name) != compute_roles.end()) observe_compute(event);
    if (name == "runtime.phase.device_collective") observe_collective(event);
}

void ProbeReader::observe_collective(const ProbeEvent & event) {
    using namespace trace_graph::planning;
    const auto & args = event.args;
    if (args.at("status") != "returned") return;
    const auto frame = forward_frames_.find(event.process);
    if (frame == forward_frames_.end()) {
        ++output_.counts["collective_without_forward_context"];
        return;
    }

    const auto implementation = args.at("implementation").get<std::string>();
    const bool reduction = implementation == "sglang.NpuCommunicator.all_reduce";
    const bool gather = implementation == "sglang.NpuCommunicator.all_gather";
    const auto shape = args.at("input_shape").get<std::vector<double>>();
    const auto stride = args.at("input_stride").get<std::vector<double>>();
    if ((!reduction && !gather) || shape.size() != 2 || stride.size() != 2 || stride[0] != shape[1] || stride[1] != 1 || args.at("input_device_type") != "npu"
        || (reduction && args.at("operation") != "sum") || (gather && args.at("gather_dim") != 1 && args.at("gather_dim") != -1)) {
        ++output_.counts["collective_unsupported_implementation_or_layout"];
        return;
    }

    const auto members = args.at("group_members").get<std::vector<Index>>();
    const auto rank = args.at("global_rank").get<std::int64_t>();
    const auto sequence = args.at(reduction ? "observed_all_reduce_index" : "observed_all_gather_index").get<std::int64_t>();
    if (rank < 0 || sequence < 0 || members.empty() || members.size() != args.at("world_size").get<Index>()
        || std::ranges::count(members, static_cast<Index>(rank)) != 1
        || std::ranges::any_of(args.at("group_members"), [](const auto & member) { return member.template get<std::int64_t>() < 0; }))
        throw std::runtime_error("Collective membership or sequence is invalid");

    const auto identity = args.at("group").get<std::string>();
    const auto [group, inserted] = collective_groups_.try_emplace(std::pair(identity, members), output_.collective_groups.size());
    if (inserted) output_.collective_groups.push_back({ identity, members });
    const auto phase = output_.compute_frames[frame->second].phase == Phase::Prefill ? "prefill." : "decode.";
    const auto operation = reduction ? std::string(phase) + "all_reduce" : "collective.all_gather";
    const auto bytes = args.at("element_bytes").get<double>();
    const auto basis = args.value("last_token_projection", false) ? RowBasis::BatchSize : RowBasis::QueryTokens;
    const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + event.process + ":" + event.thread);
    output_.collectives.push_back({
        group->second,
        static_cast<Index>(rank),
        static_cast<Index>(sequence),
        frame->second,
        resource,
        event.begin_ns,
        event.end_ns,
        event.record,
        { { operation, implementation + ":" + args.at("input_dtype").get<std::string>(), "collective_completion_interval" },
                 { { "rows", shape[0] },
                 { "width", shape[1] },
                 { "element_bytes", bytes },
                 { "participants", static_cast<double>(members.size()) },
                 { "logical_tensor_bytes", shape[0] * shape[1] * bytes } }                                                                                                  },
        {                                                                                                             basis, args.at("rows_per_token").get<double>() }
    });
}

trace_graph::planning::PrefixId ProbeReader::token_path(const nlohmann::json & dictionary) {
    const auto identity = dictionary.at("token_path_id").get<std::string>();
    if (!dictionary.contains("token_ids")) return token_descriptors_.at(identity);

    // Native probes declare each payload once, then refer to its dictionary ID.
    // Resolve that protocol here; target identity uses the actual token ancestry.
    const auto tokens = dictionary.at("token_ids").get<std::vector<std::int64_t>>();
    const auto path = output_.token_paths.intern(tokens);
    const auto [entry, inserted] = token_descriptors_.try_emplace(identity, path);
    if (!inserted && entry->second != path) throw std::runtime_error("Source token dictionary reuses an ID for different content");
    return path;
}

const AttentionDescriptor & ProbeReader::attention_descriptor(const ProbeEvent & event, trace_graph::planning::Phase phase) {
    using namespace trace_graph::planning;
    const auto & args = event.args;
    const auto key = std::tuple(event.process, phase, args.at("descriptor_id").get<Index>());
    if (!args.at("descriptor_present").get<bool>()) return attention_descriptors_.at(key);

    AttentionDescriptor result{
        std::nullopt, {}, args.at("page_size_tokens").get<double>(), args.at("query_shape").at(0).get<double>(), std::nullopt, args.at("layer_id").get<Index>()
    };
    if (!args.at("extend_query_lengths").is_null()) result.query_tokens = args.at("extend_query_lengths").get<std::vector<double>>();
    const auto d = args.at("query_key_head_dim").get<double>(), v = args.at("value_head_dim").get<double>();
    // These are the eager Ascend FIA branches; graph, sliding-window and
    // grouped requests need their own execution and work descriptions.
    const bool supported =
        args.at("use_fia").get<bool>() && !args.at("use_mla").get<bool>() && args.at("use_alibi") != true && !args.at("graph_mode").get<bool>()
        && !args.at("cross_attention").get<bool>() && !args.at("encoder_present").get<bool>() && !args.at("sinks_present").get<bool>()
        && !args.at("mode_is_target_verify").get<bool>() && !args.at("mode_is_draft_extend").get<bool>() && args.at("sliding_window_size").get<int>() == -1
        && args.at("attention_cp_size").get<int>() == 1 && args.at("batch_size").get<int>() == 1 && args.at("value_heads") == args.at("key_heads")
        && (phase == Phase::Prefill ? ((d == v && (d == 128 || d == 192 || d == 256)) || (d == 192 && v == 128))
                                    : args.at("fia_torch_ops_dispatch") == "batch_invariant");
    const auto writes = attention_writes(args);
    if (supported && writes) {
        const auto implementation = phase == Phase::Prefill ? "ascend.fia.TND" : "ascend.fia.batch_invariant.BSND";
        result.operation = ComputeOperation{
            implementation,
            AttentionGeometry{ args.at("query_heads").get<double>(), args.at("key_heads").get<double>(), d, v, args.at("causal_attention").get<bool>() }
        };
        result.cache_writes = *writes;
    }
    const auto [stored, inserted] = attention_descriptors_.try_emplace(key, result);
    if (!inserted && stored->second != result) throw std::runtime_error("Attention descriptor ID is redefined with different work");
    return stored->second;
}

void ProbeReader::observe_compute(const ProbeEvent & event) {
    const auto & args = event.args;
    if (args.at("status") != "returned") return;
    if (event.name == "runtime.phase.logits_pruning" && args.at("passthrough").get<bool>()) {
        ++output_.counts["compute_view_only.logits_pruning"];
        return;
    }
    const auto context = forward_frames_.find(event.process);
    if (context == forward_frames_.end()) {
        ++output_.counts["compute_work_without_forward_context"];
        return;
    }
    auto & frame = output_.compute_frames[context->second];
    const AttentionDescriptor * descriptor = nullptr;
    if (event.name == "runtime.phase.attention") {
        const auto phase = frame.phase == trace_graph::planning::Phase::Prefill ? "prefill" : "decode";
        if (args.at("phase") != phase) {
            ++output_.counts["compute_frame_phase_mismatch"];
            return;
        }
        descriptor = &attention_descriptor(event, frame.phase);
        frame.page_tokens = descriptor->page_tokens;
        if (frame.phase == trace_graph::planning::Phase::Prefill) frame.query_tokens = descriptor->query_tokens;
    }

    auto observed = read_compute(event, frame, descriptor);
    if (observed.empty()) {
        ++output_.counts["compute_work_unsupported_geometry." + event.name];
        return;
    }

    // The probe supplies geometry and provenance, not a device measurement.
    // The independent provider estimates service; this CPU envelope is not charged.
    const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + event.process + ":" + event.thread);
    std::optional<Index> layer;
    if (descriptor) layer = descriptor->layer;
    else if (args.contains("model_layer_index") && !args.at("model_layer_index").is_null()) layer = args.at("model_layer_index").get<Index>();
    else if (args.contains("layer_id") && !args.at("layer_id").is_null()) layer = args.at("layer_id").get<Index>();
    std::vector<trace_graph::planning::Work> work;
    std::vector<trace_graph::planning::ComputePart> recipe;
    for (auto & part : observed) {
        for (auto & operation : part.operation.describe(frame.phase, part.input)) work.push_back(std::move(operation));
        recipe.push_back({ std::move(part.operation), part.rows });
    }
    const auto caller = event.name + ":" + args.value("implementation", std::string{});
    auto calls = read_dense_calls(event, recipe);
    const auto description = output_.describe_work(std::move(work), std::move(recipe), caller, std::move(calls.parameters));
    auto & definition = output_.work_descriptions[description];
    if (!definition.calls) definition.calls = std::move(calls.definition);
    output_.work_observations.push_back({ description, resource, event.begin_ns, event.end_ns, event.record, context->second, layer });
}

} // namespace markov::adapters::sglang
