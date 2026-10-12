#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/source_dag/call_regions.hpp"
#include "markov/trace_graph/source_dag/operator_envelopes.hpp"

#include <bit>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace markov::trace_graph::source_dag {
namespace {

std::optional<structure::InputBuffersShape> input_buffer_shape(const core::TraceEvent & event) {
    const auto fields = nlohmann::json::parse(event.args_json_view());
    if (fields.at("batch_size") != 1 || fields.at("pipeline_inputs_present") != false) return std::nullopt;
    const auto rows = fields.at("input_tokens").get<int64_t>();
    if (rows <= 0) return std::nullopt;

    structure::InputBuffersShape shape{ static_cast<uint64_t>(rows), {} };
    std::map<std::pair<std::string, std::string>, size_t> conversion_groups;
    for (const auto & slot : fields.at("slots")) {
        if (slot.at("custom_source") != false || slot.at("post_fill") != false) return std::nullopt;
        if (slot.at("copy_from_batch") == false || slot.at("source_tensor") == false) continue;
        const auto axis = slot.at("axis").get<std::string>();
        if ((axis != "tokens" && axis != "bs") || slot.at("custom_slice") != false) return std::nullopt;
        const auto elements = axis == "tokens" ? rows : 1;
        if (slot.at("source_shape") != nlohmann::json::array({ elements }) || slot.at("source_stride") != nlohmann::json::array({ 1 })
            || slot.at("buffer_shape").size() != 1 || slot.at("buffer_shape").at(0).get<int64_t>() < elements
            || slot.at("buffer_stride") != nlohmann::json::array({ 1 }))
            return std::nullopt;

        const auto source_dtype = slot.at("source_dtype").get<std::string>();
        const auto destination_dtype = slot.at("buffer_dtype").get<std::string>();
        if (slot.at("source_device") != slot.at("buffer_device")) return std::nullopt;
        if (slot.at("buffer_device") == "cpu") {
            if (axis != "bs" || source_dtype != destination_dtype) return std::nullopt;
            ++shape.host_copy_count;
            continue;
        }
        if (slot.at("buffer_device") != "npu" || destination_dtype != "torch.int64" || (source_dtype != "torch.int64" && source_dtype != "torch.int32"))
            return std::nullopt;
        // Mixed-dtype foreach falls back to one copy per tensor. This rule
        // covers its single-copy case, not an unknown multi-submit call.
        if (source_dtype != destination_dtype && ++conversion_groups[{ source_dtype, destination_dtype }] > 1) return std::nullopt;
        shape.copies.push_back({ slot.at("name").get<std::string>(), source_dtype, destination_dtype, axis == "tokens" });
    }
    return shape.copies.empty() ? std::nullopt : std::optional{ std::move(shape) };
}

} // namespace

std::vector<DenseRegion> observe_dense_regions(const modules::hicache::patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                               std::map<std::string, size_t> & issues) {
    const auto & events = source.graph().runtime_observations();
    const detail::OperatorEnvelopeIndex operators(events);
    std::map<std::string, std::set<std::string>> devices_by_pid;
    for (const auto & event : events)
        if (event.name == "runtime.phase.attention_metadata" && !event.arg("device_name").empty()) devices_by_pid[event.pid].insert(event.arg("device_name"));

    std::vector<DenseRegion> result;
    for (size_t i = 0; i < events.size(); ++i) {
        const auto & event = events[i];
        if (event.source_channel == core::TraceSourceChannel::PythonProbe && event.name == "runtime.phase.input_buffers" && event.arg("status") == "returned"
            && event.arg("implementation") == "sglang.EagerRunner.load_batch" && event.arg("copy_enabled") == "true") {
            const auto shape = input_buffer_shape(event);
            if (!shape) {
                ++issues["input_buffer_layout_not_supported"];
                continue;
            }
            auto region = observe_submitted_call_region(source, queues, i);
            if (region.caller.status != "ready" || region.caller.has_node_overlap) {
                ++issues["input_buffer_caller_not_isolated"];
                continue;
            }
            result.push_back({ std::move(region), operators.inside(event), "sglang.EagerRunner.load_batch", *shape });
            continue;
        }
        if (event.source_channel == core::TraceSourceChannel::PythonProbe && event.name == "runtime.phase.logits_pruning" && event.arg("status") == "returned"
            && event.arg("implementation") == "sglang.LogitsProcessor._get_pruned_states" && event.arg("last_token_selection") == "true") {
            const auto fields = nlohmann::json::parse(event.args_json_view());
            if (!fields.contains("input_shape") || !fields.at("input_shape").is_array() || fields.at("input_shape").size() != 2) continue;
            const auto rows = fields.at("input_shape").at(0).get<int64_t>();
            const auto width = fields.at("input_shape").at(1).get<int64_t>();
            if (rows <= 0 || width <= 0 || event.arg("input_dtype").empty() || event.arg("output_dtype") != event.arg("input_dtype")
                || event.arg("before_norm_present") != "false" || !fields.contains("auxiliary_count") || fields.at("auxiliary_count") != 0
                || !fields.contains("padded_static_len") || fields.at("padded_static_len") != -1 || event.arg("length_dtype") != "torch.int32"
                || event.arg("length_device_type") != "npu")
                continue;
            const auto matches = [&](const char * key, const nlohmann::json & expected) { return fields.contains(key) && fields.at(key) == expected; };
            if (!matches("length_shape", nlohmann::json::array({ 1 })) || !matches("input_stride", nlohmann::json::array({ width, 1 }))
                || !matches("output_shape", nlohmann::json::array({ 1, width })) || !matches("output_stride", nlohmann::json::array({ width, 1 })))
                continue;

            auto region = observe_submitted_call_region(source, queues, i);
            if (region.caller.status != "ready" || region.caller.has_node_overlap) continue;
            result.push_back({
                std::move(region),
                operators.inside(event),
                event.arg("input_dtype"),
                structure::LogitsPruningShape{ static_cast<uint64_t>(rows), static_cast<uint64_t>(width) }
            });
            continue;
        }
        if (event.source_channel == core::TraceSourceChannel::PythonProbe && event.name == "runtime.phase.greedy_sampling" && event.arg("status") == "returned"
            && event.arg("implementation") == "sglang.Sampler.greedy_argmax") {
            const auto fields = nlohmann::json::parse(event.args_json_view());
            if (!fields.contains("input_shape") || !fields.at("input_shape").is_array() || fields.at("input_shape").size() != 2
                || fields.at("input_shape").at(0) != 1 || event.arg("input_dtype") != "torch.float32" || event.arg("output_dtype") != "torch.int64")
                continue;
            const auto width = fields.at("input_shape").at(1).get<int64_t>();
            if (width <= 0 || !fields.contains("input_stride") || fields.at("input_stride") != nlohmann::json::array({ width, 1 })
                || !fields.contains("output_shape") || fields.at("output_shape") != nlohmann::json::array({ 1 }) || !fields.contains("output_stride")
                || fields.at("output_stride") != nlohmann::json::array({ 1 }))
                continue;

            const auto reductions = match_submitted_operator_envelopes(source,
                                                                       queues,
                                                                       operators.inside(event),
                                                                       {
                                                                           { 0, "aten::argmax" }
            });
            if (reductions.size() != 1) {
                ++issues["greedy_sampling_operator_correspondence_unavailable"];
                for (const auto observation : operators.inside(event)) {
                    if (events[observation].name != "aten::argmax") continue;
                    const auto candidate = observe_submitted_call_region(source, queues, observation);
                    if (candidate.caller.status != "ready") ++issues["greedy_sampling_caller_" + candidate.caller.status];
                    if (candidate.caller.has_node_overlap) ++issues["greedy_sampling_caller_overlap"];
                    if (candidate.submitted_nodes.empty()) ++issues["greedy_sampling_without_submitted_work"];
                }
                continue;
            }

            const auto reduction = reductions.at(0);
            auto region = observe_submitted_call_region(source, queues, reduction);
            if (region.caller.status != "ready" || region.caller.has_node_overlap) continue;
            std::string implementation = "aten.argmax:torch.float32:dim=-1";
            const auto device = devices_by_pid.find(event.pid);
            if (device != devices_by_pid.end() && device->second.size() == 1 && device->second.begin()->starts_with("Ascend910B"))
                implementation += ":ascend910b";
            result.push_back({
                std::move(region),
                operators.inside(events[reduction]),
                std::move(implementation),
                structure::GreedySamplingShape{ 1, static_cast<uint64_t>(width) },
                i
            });
            continue;
        }
        if (event.source_channel == core::TraceSourceChannel::PythonProbe && event.name == "runtime.phase.logits_output" && event.arg("status") == "returned"
            && event.arg("implementation") == "sglang.LogitsProcessor._copy_logits_to_buffer" && event.arg("last_token_projection") == "true") {
            const auto fields = nlohmann::json::parse(event.args_json_view());
            if (!fields.contains("input_shape") || !fields.at("input_shape").is_array() || fields.at("input_shape").size() != 2
                || fields.at("input_shape").at(0) != 1)
                continue;
            const auto input_width = fields.at("input_shape").at(1).get<int64_t>();
            const auto width = event.arg_u64("vocab_size");
            const auto dtype = event.arg("input_dtype"), buffer = event.arg("output_buffer_present");
            if (input_width <= 0 || !width || width > static_cast<uint64_t>(input_width) || dtype.empty() || event.arg("output_dtype") != "torch.float32"
                || (buffer != "true" && buffer != "false"))
                continue;
            const bool copies = buffer == "true" || dtype != "torch.float32";
            const auto matches = [&](const char * key, const nlohmann::json & expected) { return fields.contains(key) && fields.at(key) == expected; };
            if (!matches("input_stride", nlohmann::json::array({ input_width, 1 })) || !matches("output_shape", nlohmann::json::array({ 1, width }))
                || !matches("output_stride", nlohmann::json::array({ copies ? width : static_cast<uint64_t>(input_width), 1 })))
                continue;

            auto region = observe_submitted_call_region(source, queues, i);
            if (region.caller.status != "ready" || region.caller.has_node_overlap) continue;
            const std::string backend = buffer == "true" ? "aten.copy_:" : copies ? "aten._to_copy:" : "aten.slice_view:";
            result.push_back({
                std::move(region),
                operators.inside(event),
                backend + "logits:" + dtype + ":torch.float32",
                structure::LogitsOutputShape{ 1, static_cast<uint64_t>(input_width), width, copies }
            });
            continue;
        }
        if (event.source_channel == core::TraceSourceChannel::PythonProbe && event.name == "runtime.phase.embedding_forward"
            && event.arg("status") == "returned" && event.arg("implementation") == "sglang.VocabParallelEmbedding.forward"
            && event.arg("quant_implementation") == "sglang.srt.layers.quantization.unquant.UnquantizedEmbeddingMethod" && event.arg_u64("tp_size") > 1) {
            const auto fields = nlohmann::json::parse(event.args_json_view());
            if (!fields.contains("output_shape") || !fields.at("output_shape").is_array() || fields.at("output_shape").size() != 2) continue;
            const auto rows = fields.at("output_shape").at(0).get<int64_t>();
            const auto width = fields.at("output_shape").at(1).get<int64_t>();
            if (rows <= 0 || width <= 0 || event.arg("output_dtype").empty() || !fields.contains("input_shape")
                || fields.at("input_shape") != nlohmann::json::array({ rows }) || !fields.contains("output_stride")
                || fields.at("output_stride") != nlohmann::json::array({ width, 1 }))
                continue;

            std::vector<size_t> fills;
            for (const auto index : operators.inside(event))
                if (events[index].name == "aten::masked_fill_") fills.push_back(index);
            // One known framework call after lookup. Multiple fills require
            // explicit roles; neither occurrence order nor nearest time proves
            // that an arbitrary nested fill is the embedding output mask.
            if (fills.size() != 1) continue;
            auto region = observe_submitted_call_region(source, queues, fills.front());
            if (region.caller.status != "ready" || region.caller.has_node_overlap) continue;
            result.push_back({
                std::move(region),
                operators.inside(events[fills.front()]),
                "aten.masked_fill_:embedding:" + event.arg("output_dtype"),
                structure::EmbeddingOutputMaskShape{ static_cast<uint64_t>(rows), static_cast<uint64_t>(width) },
                i
            });
            continue;
        }
        if (event.source_channel == core::TraceSourceChannel::PythonProbe && event.name == "runtime.phase.embedding_mask" && event.arg("status") == "returned"
            && event.arg("implementation") == "sglang.get_masked_input_and_mask.npu_eager") {
            const auto fields = nlohmann::json::parse(event.args_json_view());
            if (!fields.contains("input_shape") || !fields.at("input_shape").is_array() || fields.at("input_shape").size() != 1) continue;
            const auto rows = fields.at("input_shape").at(0).get<int64_t>();
            if (rows <= 0 || event.arg("input_dtype") != "torch.int64" || event.arg("output_dtype") != "torch.int64" || event.arg("mask_dtype") != "torch.bool")
                continue;
            const auto matches = [&](const char * key, const nlohmann::json & expected) { return fields.contains(key) && fields.at(key) == expected; };
            if (!matches("input_stride", nlohmann::json::array({ 1 })) || !matches("output_shape", nlohmann::json::array({ rows }))
                || !matches("mask_shape", nlohmann::json::array({ rows })))
                continue;

            auto region = observe_submitted_call_region(source, queues, i);
            if (region.caller.status != "ready" || region.caller.has_node_overlap) continue;
            result.push_back(
                { std::move(region), operators.inside(event), "sglang.embedding_mask:int64", structure::EmbeddingMaskShape{ static_cast<uint64_t>(rows) } });
            continue;
        }
        if (event.source_channel == core::TraceSourceChannel::PythonProbe && event.name == "runtime.phase.embedding" && event.arg("status") == "returned"
            && event.arg("implementation") == "torch.nn.functional.embedding") {
            const auto fields = nlohmann::json::parse(event.args_json_view());
            if (!fields.contains("indices_shape") || !fields.at("indices_shape").is_array() || fields.at("indices_shape").size() != 1
                || !fields.contains("weight_shape") || !fields.at("weight_shape").is_array() || fields.at("weight_shape").size() != 2)
                continue;
            const auto rows = fields.at("indices_shape").at(0).get<int64_t>();
            const auto vocabulary = fields.at("weight_shape").at(0).get<int64_t>();
            const auto width = fields.at("weight_shape").at(1).get<int64_t>();
            const auto dtype = event.arg("weight_dtype");
            if (rows <= 0 || vocabulary <= 0 || width <= 0 || dtype.empty() || event.arg("indices_dtype") != "torch.int64" || event.arg("output_dtype") != dtype
                || event.arg("indices_device_type") != "npu" || event.arg("weight_device_type") != "npu")
                continue;
            const auto matches = [&](const char * key, const nlohmann::json & expected) { return fields.contains(key) && fields.at(key) == expected; };
            if (!matches("indices_stride", nlohmann::json::array({ 1 })) || !matches("weight_stride", nlohmann::json::array({ width, 1 }))
                || !matches("output_shape", nlohmann::json::array({ rows, width })))
                continue;

            auto region = observe_submitted_call_region(source, queues, i);
            if (region.caller.status != "ready" || region.caller.has_node_overlap) continue;
            result.push_back({
                std::move(region),
                operators.inside(event),
                "torch.embedding:" + dtype + ":index=torch.int64",
                structure::EmbeddingShape{ static_cast<uint64_t>(rows), static_cast<uint64_t>(vocabulary), static_cast<uint64_t>(width) }
            });
            continue;
        }
        if (event.source_channel == core::TraceSourceChannel::PythonProbe && event.name == "runtime.phase.qk_reshape" && event.arg("status") == "returned"
            && event.arg("implementation") == "sglang.reshape_for_qk_norm") {
            const auto fields = nlohmann::json::parse(event.args_json_view());
            bool complete = true;
            for (const auto * key : { "input_shape", "input_stride", "output_shape", "output_stride" })
                complete &= fields.contains(key) && fields.at(key).is_array() && fields.at(key).size() == 2;
            if (!complete) continue;
            const auto rows = fields.at("input_shape").at(0).get<int64_t>();
            const auto width = fields.at("input_shape").at(1).get<int64_t>();
            const auto row_stride = fields.at("input_stride").at(0).get<int64_t>();
            const auto head = event.arg_u64("head_dim");
            if (rows <= 0 || width <= 0 || row_stride < width || head == 0 || static_cast<uint64_t>(width) % head || fields.at("input_stride").at(1) != 1
                || event.arg("input_dtype").empty() || event.arg("output_dtype") != event.arg("input_dtype"))
                continue;

            const structure::QkReshapeShape shape{ static_cast<uint64_t>(rows), static_cast<uint64_t>(width) / head, head, static_cast<uint64_t>(row_stride) };
            const auto output_rows = core::checked_multiply_u64(shape.rows, shape.heads, "Q/K reshape rows overflow");
            const auto output_stride = shape.heads == 1 ? shape.input_row_stride : head;
            if (fields.at("output_shape") != nlohmann::json::array({ output_rows, head }) || fields.at("output_stride").at(1) != 1
                || (output_rows > 1 && fields.at("output_stride").at(0) != output_stride)
                || event.arg("storage_shared") != (shape.copies_input() ? "false" : "true"))
                continue;

            auto region = observe_submitted_call_region(source, queues, i);
            if (region.caller.status != "ready" || region.caller.has_node_overlap) continue;
            result.push_back({ std::move(region), operators.inside(event), "aten.clone:qk_reshape:" + event.arg("input_dtype"), shape });
            continue;
        }
        if (event.source_channel == core::TraceSourceChannel::PythonProbe && event.name == "runtime.phase.rotary" && event.arg("status") == "returned"
            && event.arg("implementation") == "sglang.RotaryEmbedding.forward_npu") {
            const auto fields = nlohmann::json::parse(event.args_json_view());
            bool complete = true;
            for (const auto * key : { "query_input_shape", "key_input_shape", "table_shape" })
                complete &= fields.contains(key) && fields.at(key).is_array() && fields.at(key).size() == 2;
            if (!complete) continue;

            const auto rows = fields.at("query_input_shape").at(0).get<int64_t>();
            const auto q = fields.at("query_input_shape").at(1).get<int64_t>();
            const auto kv = fields.at("key_input_shape").at(1).get<int64_t>();
            const auto table_rows = fields.at("table_shape").at(0).get<int64_t>();
            const auto head = event.arg_u64("head_dim");
            if (rows <= 0 || q <= 0 || kv <= 0 || table_rows <= 0 || head == 0 || head % 2 || static_cast<uint64_t>(q) % head
                || static_cast<uint64_t>(kv) % head || event.arg_u64("rotary_dim") != head)
                continue;
            const auto dtype = event.arg("query_input_dtype"), table_dtype = event.arg("table_dtype"), index_dtype = event.arg("positions_dtype");
            const bool native_float_table = dtype == "torch.bfloat16" && table_dtype == "torch.float32";
            const auto fused = event.arg("fused_rope_available");
            if (native_float_table
                && ((fused != "false" && fused != "true") || event.arg("cached_position_table") != "false" || event.arg("native_rotary_uncompiled") != "true"))
                continue;
            if (dtype.empty() || table_dtype.empty() || (index_dtype != "torch.int64" && index_dtype != "torch.int32")
                || event.arg("offsets_present") != "false" || event.arg("fused_kv_write") != "false")
                continue;
            const auto shape_is = [&](const char * key, const nlohmann::json & expected) { return fields.contains(key) && fields.at(key) == expected; };
            if (!shape_is("positions_shape", nlohmann::json::array({ rows })) || !shape_is("positions_stride", nlohmann::json::array({ 1 }))
                || !shape_is("table_shape", nlohmann::json::array({ table_rows, head })) || !shape_is("table_stride", nlohmann::json::array({ head, 1 }))
                || !shape_is("key_input_shape", nlohmann::json::array({ rows, kv })) || !shape_is("query_input_stride", nlohmann::json::array({ q, 1 }))
                || !shape_is("key_input_stride", nlohmann::json::array({ kv, 1 })) || !shape_is("query_shape", nlohmann::json::array({ rows, q }))
                || !shape_is("key_shape", nlohmann::json::array({ rows, kv })) || event.arg("key_input_dtype") != dtype || event.arg("query_dtype") != dtype
                || event.arg("key_dtype") != dtype)
                continue;
            for (const auto * prefix : { "positions", "query_input", "key_input", "table" })
                complete &= event.arg(std::string(prefix) + "_device_type") == "npu";
            const auto neox = event.arg("is_neox_style");
            if (!complete || (neox != "true" && neox != "false")) continue;

            const structure::RotaryShape shape{ static_cast<uint64_t>(rows),       static_cast<uint64_t>(q), static_cast<uint64_t>(kv), head,
                                                static_cast<uint64_t>(table_rows), neox == "true",           native_float_table,        fused == "true" };
            if (structure::selects_fused_rotary(shape)) continue;

            auto region = observe_submitted_call_region(source, queues, i);
            if (region.caller.status != "ready" || region.caller.has_node_overlap) continue;
            const std::string backend = native_float_table ? "sglang.native_rope:" : "torch_npu.mrope:";
            result.push_back({ std::move(region), operators.inside(event), backend + dtype + ":table=" + table_dtype + ":index=" + index_dtype, shape });
            continue;
        }
        if (event.source_channel == core::TraceSourceChannel::PythonProbe && event.name == "runtime.phase.rotary_table" && event.arg("status") == "returned"
            && event.arg("implementation") == "sglang.RotaryEmbedding.get_cos_sin_with_position") {
            const auto fields = nlohmann::json::parse(event.args_json_view());
            if (!fields.contains("positions_shape") || !fields.at("positions_shape").is_array() || fields.at("positions_shape").size() != 1
                || !fields.contains("table_shape") || !fields.at("table_shape").is_array() || fields.at("table_shape").size() != 2)
                continue;
            const auto rows = fields.at("positions_shape").at(0).get<int64_t>();
            const auto table_rows = fields.at("table_shape").at(0).get<int64_t>();
            const auto width = fields.at("table_shape").at(1).get<int64_t>();
            if (rows <= 0 || table_rows <= 0 || width <= 0 || width % 2 || event.arg("positions_device_type") != "npu"
                || event.arg("table_device_type") != "npu")
                continue;

            const auto shape_is = [&](const char * key, const nlohmann::json & expected) { return fields.contains(key) && fields.at(key) == expected; };
            if (!shape_is("positions_stride", nlohmann::json::array({ 1 })) || !shape_is("table_stride", nlohmann::json::array({ width, 1 }))) continue;
            const auto dtype = event.arg("table_dtype"), index_dtype = event.arg("positions_dtype");
            if (dtype.empty() || (index_dtype != "torch.int64" && index_dtype != "torch.int32")) continue;
            bool complete = true;
            for (const auto * prefix : { "cos", "sin" }) {
                const std::string name(prefix);
                complete &= shape_is((name + "_shape").c_str(), nlohmann::json::array({ rows, 1, 1, width }));
                complete &= event.arg(name + "_dtype") == dtype;
            }
            if (!complete) continue;

            auto region = observe_submitted_call_region(source, queues, i);
            if (region.caller.status != "ready" || region.caller.has_node_overlap) continue;
            result.push_back({
                std::move(region),
                operators.inside(event),
                "sglang.rotary_table:" + dtype + ":index=" + index_dtype,
                structure::RotaryTableShape{ static_cast<uint64_t>(rows), static_cast<uint64_t>(width), static_cast<uint64_t>(table_rows) }
            });
            continue;
        }
        if (event.source_channel == core::TraceSourceChannel::PythonProbe && event.name == "runtime.phase.qkv_norm_rope" && event.arg("status") == "returned"
            && event.arg("implementation") == "sgl_kernel_npu.split_qkv_rmsnorm_rope") {
            const auto fields = nlohmann::json::parse(event.args_json_view());
            const auto q = event.arg_u64("query_width"), kv = event.arg_u64("kv_width"), head = event.arg_u64("head_dim");
            if (!head || !std::has_single_bit(head) || !kv || kv % head || q % kv || q == 0 || !fields.contains("input_shape")
                || !fields.at("input_shape").is_array() || fields.at("input_shape").size() != 2 || !fields.contains("sin_shape")
                || !fields.at("sin_shape").is_array() || fields.at("sin_shape").size() != 4)
                continue;
            const auto rows = fields.at("input_shape").at(0).get<int64_t>();
            const auto rope = fields.at("sin_shape").at(3).get<int64_t>();
            if (rows <= 0 || rope <= 0 || rope % 2 || static_cast<uint64_t>(rope) > head) continue;

            const auto width = core::checked_add_u64(q, core::checked_multiply_u64(kv, 2, "QKV width overflow"), "QKV width overflow");
            const auto shape_is = [&](const char * key, const nlohmann::json & expected) { return fields.contains(key) && fields.at(key) == expected; };
            if (!shape_is("input_shape", nlohmann::json::array({ rows, width })) || !shape_is("input_stride", nlohmann::json::array({ width, 1 }))
                || !shape_is("query_shape", nlohmann::json::array({ rows, q })) || !shape_is("key_shape", nlohmann::json::array({ rows, kv }))
                || !shape_is("value_shape", nlohmann::json::array({ rows, kv })))
                continue;
            bool layout = true;
            for (const auto * prefix : { "sin", "cos" }) {
                const std::string name(prefix);
                layout &= shape_is((name + "_shape").c_str(), nlohmann::json::array({ rows, 1, 1, rope }));
                const auto stride = fields.find(name + "_stride");
                layout &= stride != fields.end() && stride->is_array() && stride->size() == 4 && stride->at(3) == 1 && (rows == 1 || stride->at(0) == rope);
            }
            for (const auto * prefix : { "query", "key" }) {
                const std::string name(prefix);
                layout &= shape_is((name + "_weight_shape").c_str(), nlohmann::json::array({ head }));
                layout &= shape_is((name + "_weight_stride").c_str(), nlohmann::json::array({ 1 }));
                layout &= shape_is((name + "_bias_shape").c_str(), nullptr);
            }
            const auto dtype = event.arg("input_dtype"), weight_dtype = event.arg("query_weight_dtype"), rope_dtype = event.arg("sin_dtype");
            if (!layout || dtype.empty() || weight_dtype.empty() || rope_dtype.empty() || event.arg("query_dtype") != dtype || event.arg("key_dtype") != dtype
                || event.arg("value_dtype") != dtype || event.arg("key_weight_dtype") != weight_dtype || event.arg("cos_dtype") != rope_dtype
                || !fields.contains("epsilon") || !fields.at("epsilon").is_number())
                continue;
            const auto epsilon = fields.at("epsilon").get<double>();
            const auto queue = event.arg("triton_task_queue_enabled"), neox = event.arg("is_neox_style");
            if (!std::isfinite(epsilon) || epsilon <= 0 || (queue != "true" && queue != "false") || (neox != "true" && neox != "false")) continue;

            auto region = observe_submitted_call_region(source, queues, i);
            if (region.caller.status != "ready" || region.caller.has_node_overlap) continue;
            result.push_back({
                std::move(region),
                operators.inside(event),
                "sgl_kernel_npu.split_qkv_rmsnorm_rope:" + dtype + ":weight=" + weight_dtype + ":rope=" + rope_dtype,
                structure::QkvRopeShape{ static_cast<uint64_t>(rows), q, kv, head, static_cast<uint64_t>(rope), epsilon, queue == "true", neox == "true" }
            });
            continue;
        }
        const bool logits = event.name == "runtime.phase.logits_projection" && event.arg("implementation") == "sglang.LogitsProcessor._compute_lm_head"
                            && event.arg("last_token_projection") == "true";
        const bool linear = logits || (event.name == "runtime.phase.linear" && event.arg("implementation") == "torch.nn.functional.linear");
        const bool gate = event.name == "runtime.phase.gated_activation" && event.arg("implementation") == "torch_npu.npu_swiglu";
        const bool norm = event.name == "runtime.phase.normalization"
                          && (event.arg("implementation") == "torch_npu.npu_rms_norm" || event.arg("implementation") == "torch_npu.npu_add_rms_norm");
        if (event.source_channel != core::TraceSourceChannel::PythonProbe || (!linear && !gate && !norm) || event.arg("status") != "returned") continue;

        const auto fields = nlohmann::json::parse(event.args_json_view());
        bool complete = true;
        for (const auto key : { "input_shape", "input_stride", "output_shape", "weight_shape", "weight_stride" }) {
            if (!linear && (std::string_view(key) == "weight_shape" || std::string_view(key) == "weight_stride")) continue;
            if (!fields.contains(key) || !fields.at(key).is_array() || fields.at(key).size() != 2) {
                complete = false;
                break;
            }
            for (const auto & dimension : fields.at(key)) complete &= dimension.is_number_integer() && dimension.get<int64_t>() > 0;
        }
        if (!complete) continue;

        const auto input = fields.at("input_shape").get<std::vector<uint64_t>>();
        if (logits && input[0] != 1) continue;
        const auto output = fields.at("output_shape").get<std::vector<uint64_t>>();
        const auto input_stride = fields.at("input_stride").get<std::vector<uint64_t>>();
        const auto dtype = event.arg("input_dtype");
        if (dtype.empty() || dtype != event.arg("output_dtype")) continue;

        auto region = observe_submitted_call_region(source, queues, i);
        if (region.caller.status != "ready" || region.caller.has_node_overlap) continue;
        if (norm) {
            // Additional residual addition is a separate operation, not part
            // of this fused primitive. Do not fold its cost into RMSNorm.
            if (output != input || event.arg("post_residual_addition") != "false" || !fields.contains("weight_shape")
                || fields.at("weight_shape") != nlohmann::json::array({ input[1] }) || !fields.contains("weight_stride")
                || fields.at("weight_stride") != nlohmann::json::array({ 1 }) || event.arg("weight_dtype").empty() || !fields.contains("epsilon")
                || !fields.at("epsilon").is_number())
                continue;
            const auto epsilon = fields.at("epsilon").get<double>();
            if (!std::isfinite(epsilon) || epsilon <= 0) continue;

            const bool residual = event.arg("implementation") == "torch_npu.npu_add_rms_norm";
            const auto split = event.arg("split_residual_norm");
            if (residual && split != "true" && split != "false") continue;
            if (residual) {
                if (!fields.contains("residual_shape") || fields.at("residual_shape") != fields.at("input_shape") || !fields.contains("residual_stride")
                    || fields.at("residual_stride") != fields.at("input_stride") || event.arg("residual_dtype") != dtype
                    || !fields.contains("residual_output_shape") || fields.at("residual_output_shape") != fields.at("input_shape")
                    || event.arg("residual_output_dtype") != dtype)
                    continue;
                if (split == "true"
                    && (!fields.contains("residual_output_stride") || fields.at("residual_output_stride") != nlohmann::json::array({ input[1], 1 })))
                    continue;
            }
            else if (!fields.contains("residual_shape") || !fields.at("residual_shape").is_null()) continue;

            const std::string implementation =
                residual ? (split == "true" ? "sglang.batch_invariant_add_rms_norm:" : "torch_npu.add_rms_norm:") : "torch_npu.rms_norm:";
            result.push_back({
                std::move(region),
                operators.inside(event),
                implementation + dtype + ":weight=" + event.arg("weight_dtype"),
                structure::NormalizationShape{ input[0], input[1], input_stride[0], input_stride[1], epsilon, residual, split == "true" }
            });
            continue;
        }
        if (gate) {
            if (input[1] % 2 != 0 || output != std::vector<uint64_t>{ input[0], input[1] / 2 }) continue;

            result.push_back({
                std::move(region),
                operators.inside(event),
                "torch_npu.swiglu:" + dtype,
                structure::GatedActivationShape{ input[0], output[1], input_stride[0], input_stride[1] }
            });
            continue;
        }

        if (!fields.contains("bias_shape") || !fields.contains("bias_dtype")) continue;
        const auto weight = fields.at("weight_shape").get<std::vector<uint64_t>>();
        if (input[1] != weight[1] || output != std::vector<uint64_t>{ input[0], weight[0] } || dtype != event.arg("weight_dtype")) continue;
        const bool bias = !fields.at("bias_shape").is_null();
        if (bias && (fields.at("bias_shape") != nlohmann::json::array({ weight[0] }) || event.arg("bias_dtype") != dtype)) continue;

        const auto weight_stride = fields.at("weight_stride").get<std::vector<uint64_t>>();
        const std::string backend = logits ? "torch.matmul.logits:" : "torch.linear:";
        result.push_back({
            std::move(region),
            operators.inside(event),
            backend + dtype + (bias ? ":bias" : ":no_bias"),
            structure::LinearShape{ input[0], weight[0], input[1], input_stride[0], input_stride[1], weight_stride[0], weight_stride[1] }
        });
    }
    return result;
}

} // namespace markov::trace_graph::source_dag
