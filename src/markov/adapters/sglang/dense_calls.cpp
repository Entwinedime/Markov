#include "markov/adapters/sglang/dense.hpp"

#include <string>
#include <utility>
#include <variant>

namespace markov::adapters::sglang {
namespace {

using namespace trace_graph::planning;

enum class Kind { Simple, Normalization, Attention, InputBuffers, EmbeddingMask, Inner, Sampling, Pruning, Reshape, RotaryTable };

class DenseCalls final : public CallSequence {
public:
    DenseCalls(Kind kind, std::string caller, Features parameters) : kind_(kind), caller_(std::move(caller)), parameters_(std::move(parameters)) {}
    std::vector<NativeCall> describe(Phase phase, const AttentionInput & input, std::span<const Work> work) const override;

private:
    Kind kind_;
    std::string caller_;
    Features parameters_;
};

void cpu(std::vector<NativeCall> & calls, const std::string & operation, const std::string & implementation, Features features) {
    calls.push_back({
        { { operation, implementation, "cpu_service" }, std::move(features) },
        CallKind::Cpu,
        {}
    });
}

void queued(std::vector<NativeCall> & calls, const Work & work) { calls.push_back({ work, CallKind::Queued, { work } }); }

std::vector<NativeCall> DenseCalls::describe(Phase phase, const AttentionInput & input, std::span<const Work> work) const {
    const std::string prefix = phase == Phase::Prefill ? "prefill." : "decode.";
    std::vector<NativeCall> calls;
    if (kind_ == Kind::Reshape && work.empty()) {
        auto features = parameters_;
        const auto rows = features.at("rows_per_item") * (features.at("per_query") ? input.query_tokens : input.batch_size);
        features.erase("rows_per_item");
        features.erase("per_query");
        features["rows"] = rows;
        features["elements"] *= rows;
        cpu(calls, prefix + "qk_reshape.view", caller_, std::move(features));
        return calls;
    }
    if (kind_ == Kind::InputBuffers) {
        auto shape = parameters_;
        shape["input_rows"] = input.query_tokens;
        for (std::size_t i = 0; i < work.size(); ++i) {
            cpu(calls, prefix + (i ? "input_buffers.next_group" : "input_buffers.entry"), caller_, i ? work[i].features : shape);
            queued(calls, work[i]);
        }
        cpu(calls, prefix + "input_buffers.return", caller_, shape);
        return calls;
    }
    if (kind_ == Kind::Normalization && work.size() == 2) {
        const auto & shape = work.back().features;
        cpu(calls, prefix + "split_add_rms_norm.entry", caller_, shape);
        queued(calls, work.front());
        cpu(calls, prefix + "split_add_rms_norm.norm_prepare", caller_, shape);
        queued(calls, work.back());
        cpu(calls, prefix + "split_add_rms_norm.return", caller_, shape);
        return calls;
    }
    if (kind_ == Kind::Sampling) {
        auto parent = work.front();
        parent.kind.operation = prefix + "greedy_argmax";
        parent.kind.implementation = "aten.argmax:torch.float32:dim=-1:ascend910b";
        calls.push_back({
            std::move(parent),
            CallKind::Queued,
            { work.begin(), work.end() }
        });
        return calls;
    }
    if (kind_ == Kind::Pruning) {
        const auto & shape = work.back().features;
        cpu(calls, prefix + "logits_pruning.prepare_lengths", caller_, shape);
        auto sum = work.front();
        sum.kind.operation = prefix + "logits_pruning.cumsum";
        sum.kind.implementation = "aten.cumsum:torch.int32:dim=0:torch.int64";
        calls.push_back({
            std::move(sum),
            CallKind::Queued,
            { work[0], work[1] }
        });
        cpu(calls, prefix + "logits_pruning.prepare_last_index", caller_, shape);
        queued(calls, work[2]);
        cpu(calls, prefix + "logits_pruning.prepare_selection", caller_, shape);
        queued(calls, work[3]);
        cpu(calls, prefix + "logits_pruning.return", caller_, shape);
        return calls;
    }
    if (kind_ == Kind::EmbeddingMask || kind_ == Kind::RotaryTable || kind_ == Kind::Attention) {
        for (std::size_t i = 0; i < work.size(); ++i) {
            auto parent = work[i];
            std::vector<Work> launches{ work[i] };
            if (kind_ == Kind::EmbeddingMask && parent.kind.operation.ends_with(".promote")) {
                parent.kind.operation.resize(parent.kind.operation.size() - std::string(".promote").size());
                parent.kind.implementation = "aten.mul:sglang.embedding_mask:int64";
                launches.push_back(work[++i]);
            }
            else if (kind_ == Kind::Attention && parent.kind.operation.ends_with(".materialize")) {
                parent.kind.operation.resize(parent.kind.operation.size() - std::string(".materialize").size());
                parent.kind.implementation = "ascend.scatter_nd_update_";
                launches.push_back(work[++i]);
            }

            auto preparation = parent.features;
            auto name = parent.kind.operation + ".prepare";
            if (kind_ == Kind::Attention) {
                if (calls.empty()) name = prefix + "attention.entry";
                preparation["call_batch_size"] = input.batch_size;
                preparation["call_query_tokens"] = input.tensor_query_tokens;
            }
            cpu(calls, name, caller_, std::move(preparation));
            calls.push_back({ std::move(parent), CallKind::Queued, std::move(launches) });
        }
        const auto tail = kind_ == Kind::Attention ? "attention.return" : kind_ == Kind::EmbeddingMask ? "embedding_mask.return" : "rotary_table.return";
        const auto shape = kind_ == Kind::Attention ? Features{ { "batch_size", input.batch_size }, { "tensor_query_tokens", input.tensor_query_tokens } }
                                                   : work.back().features;
        cpu(calls, prefix + tail, caller_, shape);
        return calls;
    }
    if (kind_ == Kind::Inner) {
        queued(calls, work.front());
        return calls;
    }

    const auto & operation = work.front();
    cpu(calls, operation.kind.operation + ".entry", caller_, operation.features);
    queued(calls, operation);
    cpu(calls, operation.kind.operation + ".return", caller_, operation.features);
    return calls;
}

} // namespace

ComputeCalls read_dense_calls(const ProbeEvent & event, std::span<const ComputePart> recipe) {
    const auto & name = event.name;
    const auto & args = event.args;
    Kind kind = Kind::Simple;
    std::string caller;
    Features parameters;
    if (name == "runtime.phase.linear") caller = "sglang.UnquantizedLinearMethod.apply";
    else if (name == "runtime.phase.logits_projection") caller = "sglang.LogitsProcessor._compute_lm_head";
    else if (name == "runtime.phase.normalization") {
        kind = Kind::Normalization;
        caller = "sglang.RMSNorm.forward_npu";
    }
    else if (name == "runtime.phase.gated_activation") caller = "sglang.SiluAndMul.forward_npu";
    else if (name == "runtime.phase.embedding") caller = "sglang.UnquantizedEmbeddingMethod.embedding";
    else if (name == "runtime.phase.logits_output") caller = "sglang.LogitsProcessor._copy_logits_to_buffer";
    else if (name == "runtime.phase.rotary") caller = "sglang.RotaryEmbedding.forward_npu";
    else if (name == "runtime.phase.qkv_norm_rope") {
        // Direct Triton launches do not have the queued worker lifecycle.
        if (!args.at("triton_task_queue_enabled").get<bool>()) return {};
        caller = "sgl_kernel_npu.split_qkv_rmsnorm_rope";
    }
    else if (name == "runtime.phase.attention") {
        kind = Kind::Attention;
        caller = "sglang.AscendAttnBackend";
    }
    else if (name == "runtime.phase.embedding_mask") {
        kind = Kind::EmbeddingMask;
        caller = "sglang.get_masked_input_and_mask.npu_eager";
    }
    else if (name == "runtime.phase.rotary_table") {
        kind = Kind::RotaryTable;
        caller = "sglang.RotaryEmbedding.get_cos_sin_with_position";
    }
    else if (name == "runtime.phase.embedding_forward") kind = Kind::Inner;
    else if (name == "runtime.phase.greedy_sampling") kind = Kind::Sampling;
    else if (name == "runtime.phase.logits_pruning") {
        kind = Kind::Pruning;
        caller = "sglang.LogitsProcessor._get_pruned_states";
    }
    else if (name == "runtime.phase.input_buffers") {
        kind = Kind::InputBuffers;
        caller = "sglang.EagerRunner.load_batch";
        parameters = {
            { "device_copy_count", 0 },
            {   "host_copy_count", 0 }
        };
        for (const auto & slot : args.at("slots")) {
            if (!slot.at("copy_from_batch").get<bool>() || !slot.at("source_tensor").get<bool>()) continue;
            ++parameters[slot.at("buffer_device") == "cpu" ? "host_copy_count" : "device_copy_count"];
        }
    }
    else if (name == "runtime.phase.qk_reshape") {
        kind = Kind::Reshape;
        caller = "sglang.reshape_for_qk_norm";
        const auto & part = recipe.front();
        if (!part.rows) return {};
        const auto & shape = std::get<ReshapeGeometry>(part.operation.geometry);
        parameters = {
            {         "elements",                                       shape.width },
            {            "heads",                      shape.width / shape.head_dim },
            {         "head_dim",                                    shape.head_dim },
            { "input_row_stride",                            shape.input_row_stride },
            {    "rows_per_item",                          part.rows->rows_per_item },
            {        "per_query", part.rows->basis == RowBasis::QueryTokens ? 1 : 0 }
        };
    }
    else return {};
    return { parameters, std::make_unique<DenseCalls>(kind, std::move(caller), parameters) };
}

} // namespace markov::adapters::sglang
