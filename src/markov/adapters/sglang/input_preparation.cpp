#include "markov/adapters/sglang/input_preparation.hpp"
#include "markov/adapters/sglang/kernel_preparation.hpp"

#include <utility>
#include <vector>

namespace markov::adapters::sglang {
namespace {

using namespace trace_graph::planning;

enum class Part { RequestTable, ForwardInputs, SamplingPositions };

class InputPreparation final : public CallSequence {
public:
    InputPreparation(Part part, double table_columns, std::string sequence_dtype)
        : part_(part),
          table_columns_(table_columns),
          sequence_dtype_(std::move(sequence_dtype)) {}
    NativeSequence describe(Phase phase, const FrameInput & input, std::span<const Work>) const override;

private:
    Part part_;
    double table_columns_;
    std::string sequence_dtype_;
};

void cpu(std::vector<NativeCall> & calls, const std::string & name, const Features & shape) {
    calls.push_back({
        { { name, "sglang.batch_input_preparation", "cpu_service" }, shape },
        CallKind::Cpu,
        {}
    });
}

void queued(std::vector<NativeCall> & calls, const std::string & name, const std::string & implementation, Features shape, bool incoming_stream = false) {
    cpu(calls, name + ".prepare", shape);
    Work work{
        { name, implementation, "device_service" },
        std::move(shape)
    };
    calls.push_back({ work, CallKind::Queued, { work } });
    calls.back().incoming_stream = incoming_stream;
}

NativeSequence InputPreparation::describe(Phase phase, const FrameInput & input, std::span<const Work>) const {
    const std::string prefix = phase == Phase::Prefill ? "prefill.inputs." : "decode.inputs.";
    Features shape{
        { "batch_size", input.batch_size },
        {   "elements", input.batch_size }
    };
    std::vector<NativeCall> calls;
    if (part_ == Part::SamplingPositions) {
        queued(calls, "prefill.sampling_positions", "aten.sub:" + sequence_dtype_, shape);
        return { std::move(calls), std::nullopt };
    }
    shape["query_tokens"] = input.query_tokens;
    shape["context_tokens"] = input.context_tokens;
    shape["prefix_tokens"] = input.context_tokens - input.query_tokens;
    cpu(calls, prefix + (part_ == Part::RequestTable ? "request_table.entry" : "forward.entry"), shape);
    if (part_ == Part::RequestTable) {
        shape["table_columns"] = table_columns_;
        if (phase == Phase::Prefill) {
            queued(calls, prefix + "prefix_pointers", "aten.to.npu:uint64", shape);
            queued(calls, prefix + "request_table.write", "triton.ascend.write_req_to_token_pool", shape);
            calls.back().preparation = triton_preparation(
                "write_req_to_token_pool",
                {
                    { "table_columns", table_columns_ }
            },
                { { "pointer_int32_arguments", 2 }, { "pointer_int64_arguments", 4 }, { "pointer_uint64_arguments", 1 }, { "constant_arguments", 1 } });
        }
        else {
            queued(calls, prefix + "request_table.locations", "aten.clone:" + sequence_dtype_, shape);
            cpu(calls, prefix + "request_table.output_view", shape);
            queued(calls, prefix + "request_table.write", "aten.index_put_:int32:int64_indices", shape);
        }
    }
    else {
        if (phase == Phase::Prefill) {
            // SamplingBatchInfo creates these four arrays on the schedule stream,
            // even for greedy requests. Forward inputs are produced after handoff.
            for (const auto * name : { "temperatures", "top_ps" }) queued(calls, prefix + name, "aten.to.npu:float32", shape, true);
            queued(calls, prefix + "top_ks", "aten.to.npu:int32", shape, true);
            queued(calls, prefix + "min_ps", "aten.to.npu:float32", shape, true);
        }
        else {
            queued(calls, prefix + "sequence_increment", "aten.add:" + sequence_dtype_, shape, true);
            cpu(calls, prefix + "host_sequence_increment", shape);
            queued(calls, prefix + "original_sequence_increment", "aten.add:int32", shape, true);
        }
        const Features handoff{
            { "streams_differ", 1 }
        };
        queued(calls, prefix + "handoff.record", "ascend.event_record", handoff, true);
        queued(calls, prefix + "handoff.wait", "ascend.event_wait", handoff);
        cpu(calls, prefix + "handoff.destroy.prepare", handoff);
        calls.push_back({
            { { prefix + "handoff.destroy", "torch_npu.event_destroy", "cpu_service" }, handoff },
            CallKind::Queued,
            {}
        });
        if (phase == Phase::Prefill) {
            auto tokens = shape;
            tokens["elements"] = input.query_tokens;
            queued(calls, prefix + "token_ids", "aten.to.npu:int64", tokens);
            queued(calls, prefix + "extend_lengths", "aten.to.npu:int32", shape);
            queued(calls, prefix + "prefix_lengths", "aten.to.npu:int32", shape);
            queued(calls, prefix + "positions", "triton.ascend.compute_position", tokens);
            calls.back().preparation = triton_preparation("compute_position",
                                                          {
                                                              { "has_prefix", 1 }
            },
                                                          { { "pointer_int64_arguments", 1 }, { "pointer_int32_arguments", 3 }, { "constant_arguments", 1 } });
        }
        else {
            queued(calls, prefix + "positions.subtract", "aten.sub:" + sequence_dtype_, shape);
            queued(calls, prefix + "positions.clamp", "aten.clamp_min:" + sequence_dtype_, shape);
        }
    }
    cpu(calls, prefix + (part_ == Part::RequestTable ? "request_table.return" : "forward.return"), shape);
    return { std::move(calls), std::nullopt };
}

std::unique_ptr<const CallSequence> read(const nlohmann::json & metadata, Part part) {
    const auto & shape = metadata.at("request_table_shape");
    const auto dtype = metadata.at("sequence_dtype").get<std::string>();
    if (metadata.at("backend_implementation") != "sglang.srt.hardware_backend.npu.attention.ascend_backend.AscendAttnBackend" || metadata.at("route") != "eager"
        || metadata.at("batch_size") != 1 || metadata.at("speculative_present") != false || metadata.at("request_table_dtype") != "torch.int32"
        || metadata.at("request_table_contiguous") != true || shape.size() != 2 || dtype != "torch.int64" || metadata.at("sequence_device_type") != "npu")
        return nullptr;
    return std::make_unique<InputPreparation>(part, shape.at(1).get<double>(), "int64");
}

} // namespace

std::unique_ptr<const CallSequence> read_request_table_write(const nlohmann::json & metadata) { return read(metadata, Part::RequestTable); }
std::unique_ptr<const CallSequence> read_forward_inputs(const nlohmann::json & metadata) { return read(metadata, Part::ForwardInputs); }
std::unique_ptr<const CallSequence> sampling_positions(const std::string & sequence_dtype) {
    if (sequence_dtype != "torch.int64") return nullptr;
    return std::make_unique<InputPreparation>(Part::SamplingPositions, 0, "int64");
}

} // namespace markov::adapters::sglang
