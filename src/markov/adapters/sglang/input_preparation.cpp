#include "markov/adapters/sglang/input_preparation.hpp"
#include "markov/adapters/sglang/kernel_preparation.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov::adapters::sglang {
namespace {

using namespace trace_graph::planning;

enum class Part { RequestTable, ForwardInputs, SamplingPositions };

class InputPreparation final : public CallSequence {
public:
    InputPreparation(Part part, double table_rows, double table_columns, std::string sequence_dtype, bool deterministic_sampling = false)
        : part_(part),
          table_rows_(table_rows),
          table_columns_(table_columns),
          sequence_dtype_(std::move(sequence_dtype)),
          deterministic_sampling_(deterministic_sampling) {}
    NativeSequence describe(Phase phase, const FrameInput & input, std::span<const Work>) const override;

private:
    Part part_;
    double table_rows_;
    double table_columns_;
    std::string sequence_dtype_;
    bool deterministic_sampling_;
};

void queued(std::vector<NativeCall> & calls, const std::string & name, const std::string & implementation, Features shape, bool incoming_stream = false,
            std::string queue_name = {}) {
    Work work{
        { name, implementation, "device_service" },
        std::move(shape)
    };
    calls.push_back({ work, CallKind::Queued, { work } });
    calls.back().incoming_stream = incoming_stream;
    calls.back().queue_name = std::move(queue_name);
}

NativeSequence sequence(std::vector<NativeCall> calls, Part part, const Features & context) {
    const auto role = part == Part::RequestTable ? "request_table" : part == Part::ForwardInputs ? "forward" : "sampling_positions";
    for (std::size_t id = 0; id < calls.size(); ++id) {
        auto caller = calls[id].work;
        caller.kind.implementation = std::string("sglang.batch_input_preparation.") + role;
        caller.kind.timing_scope = "cpu_service";
        caller.features.insert(context.begin(), context.end());

        // The first pre-submit interval includes entry; the final return
        // includes the sequence tail. Neither introduces another queue port.
        caller.features["sequence_entry"] = id == 0 ? 1 : 0;
        caller.features["sequence_exit"] = id + 1 == calls.size() ? 1 : 0;
        calls[id].caller_work = std::move(caller);
    }
    return { std::move(calls), std::nullopt };
}

void upload(std::vector<NativeCall> & calls, const std::string & name, const std::string & dtype, double element_bytes, Features shape,
            bool incoming_stream = false) {
    shape["element_bytes"] = element_bytes;
    shape["host_pinned"] = 0;
    shape["non_blocking"] = 1;
    const auto bytes = shape.at("elements") * element_bytes;
    queued(calls, name, "aten.to.npu:" + dtype, std::move(shape), incoming_stream, "acl_memcpy_host_to_device");
    auto & call = calls.back();
    call.kind = CallKind::HostUpload;
    call.work.kind.timing_scope = "cpu_service";
    call.launches.clear();
    call.host_transfer = Work{
        {     "runtime.copy",   "aclrtMemcpyAsync",        "host_to_device_interval" },
        { { "bytes", bytes }, { "host_pinned", 0 }, { "previous_work_completed", 1 } }
    };
    call.synchronization = Work{
        { "runtime.stream_sync", "aclrtSynchronizeStreamWithTimeout", "idle_stream_sync_interval" },
        { { "previous_work_completed", 1 }, { "timeout_ms", -1 } }
    };
}

NativeSequence InputPreparation::describe(Phase phase, const FrameInput & input, std::span<const Work>) const {
    const std::string prefix = phase == Phase::Prefill ? "prefill.inputs." : "decode.inputs.";
    Features shape{
        { "batch_size", input.batch_size },
        {   "elements", input.batch_size }
    };
    std::vector<NativeCall> calls;
    if (part_ == Part::SamplingPositions) {
        queued(calls, "prefill.sampling_positions", "aten.sub:" + sequence_dtype_, shape, false, "aclnnSubs");
        return sequence(std::move(calls), part_, shape);
    }
    shape["query_tokens"] = input.query_tokens;
    shape["context_tokens"] = input.context_tokens;
    shape["prefix_tokens"] = input.context_tokens - input.query_tokens;
    if (part_ == Part::RequestTable) {
        shape["table_columns"] = table_columns_;
        if (phase == Phase::Prefill) {
            upload(calls, prefix + "prefix_pointers", "uint64", 8, shape);
            queued(calls, prefix + "request_table.write", "triton.ascend.write_req_to_token_pool", shape, false, "write_req_to_token_pool_triton");
            auto & work = calls.back().launches.front();

            // The native kernels iterate over blocks of 512 token locations.
            work.features = {
                {    "batch_size",                                             input.batch_size },
                {  "query_tokens",                                           input.query_tokens },
                { "prefix_tokens",                    input.context_tokens - input.query_tokens },
                {  "query_blocks",                          std::ceil(input.query_tokens / 512) },
                { "prefix_blocks", std::ceil((input.context_tokens - input.query_tokens) / 512) },
                { "table_columns",                                               table_columns_ }
            };
            calls.back().preparation = triton_preparation(
                "write_req_to_token_pool",
                {
                    { "table_columns", table_columns_ }
            },
                { { "pointer_int32_arguments", 2 }, { "pointer_int64_arguments", 4 }, { "pointer_uint64_arguments", 1 }, { "constant_arguments", 1 } });
        }
        else {
            queued(calls, prefix + "request_table.locations", "aten.clone:" + sequence_dtype_, shape, false, "aclnnInplaceCopy");
            queued(calls, prefix + "request_table.write", "aten.index_put_:int32:int64_indices", shape, false, "aclnnIndexPutImpl");
            auto & call = calls.back();
            call.work.features = {
                {    "batch_size", input.batch_size },
                {      "elements", input.batch_size },
                {    "table_rows",      table_rows_ },
                { "table_columns",   table_columns_ }
            };

            // The supported int32 table/two-int64-index route broadcasts
            // the write inputs before IndexPutV2; both share one worker call.
            call.launches = {
                Work{ { prefix + "request_table.broadcast", "ascend.index_put.broadcast:int32:int64_indices", "device_service" }, call.work.features },
                Work{         { prefix + "request_table.write", "ascend.index_put.write:int32:int64_indices", "device_service" }, call.work.features }
            };
            call.worker_first_use = Work{
                { "runtime.cann.first_use", call.work.kind.implementation, "worker_blocking_interval" },
                call.work.features
            };
        }
    }
    else {
        if (phase == Phase::Prefill) {
            // Sampling arrays use the schedule stream even for greedy requests;
            // deterministic inference adds the seed before stream handoff.
            for (const auto * name : { "temperatures", "top_ps" }) upload(calls, prefix + name, "float32", 4, shape, true);
            upload(calls, prefix + "top_ks", "int32", 4, shape, true);
            upload(calls, prefix + "min_ps", "float32", 4, shape, true);
            if (deterministic_sampling_) upload(calls, prefix + "sampling_seed", "int64", 8, shape, true);
        }
        else {
            queued(calls, prefix + "sequence_increment", "aten.add:" + sequence_dtype_, shape, true, "aclnnAdds");
            queued(calls, prefix + "original_sequence_increment", "aten.add:int32", shape, true, "aclnnAdds");
        }
        const Features handoff{
            { "streams_differ", 1 }
        };
        queued(calls, prefix + "handoff.record", "ascend.event_record", handoff, true, "record_event");
        calls.back().device_control = DeviceControl::Record;
        queued(calls, prefix + "handoff.wait", "ascend.event_wait", handoff, false, "wait_event");
        calls.back().device_control = DeviceControl::Wait;
        calls.push_back({
            { { prefix + "handoff.destroy", "torch_npu.event_destroy", "cpu_service" }, handoff },
            CallKind::Queued,
            {}
        });
        calls.back().queue_name = "destroy_event";
        if (phase == Phase::Prefill) {
            auto tokens = shape;
            tokens["elements"] = input.query_tokens;
            upload(calls, prefix + "token_ids", "int64", 8, tokens);
            upload(calls, prefix + "extend_lengths", "int32", 4, shape);
            upload(calls, prefix + "prefix_lengths", "int32", 4, shape);
            queued(calls, prefix + "positions", "triton.ascend.compute_position", tokens, false, "compute_position_kernel");
            calls.back().launches.front().features = {
                {   "batch_size",                    input.batch_size },
                { "query_tokens",                  input.query_tokens },
                { "query_blocks", std::ceil(input.query_tokens / 512) },
                {   "has_prefix",                                   1 }
            };
            calls.back().preparation = triton_preparation("compute_position",
                                                          {
                                                              { "has_prefix", 1 }
            },
                                                          { { "pointer_int64_arguments", 1 }, { "pointer_int32_arguments", 3 }, { "constant_arguments", 1 } });
        }
        else {
            // Decode consumes the preceding iteration's sampled token through
            // the pool-indexed relay before constructing the new positions.
            auto tokens = shape;
            tokens["buffer_rows"] = table_rows_;
            queued(calls, prefix + "token_ids", "aten.index:future_tokens:int64:int64_indices", std::move(tokens), false, "aclnnIndex");
            queued(calls, prefix + "positions.subtract", "aten.sub:" + sequence_dtype_, shape, false, "aclnnSubs");
            queued(calls, prefix + "positions.clamp", "aten.clamp_min:" + sequence_dtype_, shape, false, "aclnnClamp");
        }
    }
    return sequence(std::move(calls), part_, shape);
}

std::unique_ptr<const CallSequence> read(const nlohmann::json & metadata, Part part, bool deterministic_sampling = false) {
    const auto & shape = metadata.at("request_table_shape");
    const auto dtype = metadata.at("sequence_dtype").get<std::string>();
    if (metadata.at("backend_implementation") != "sglang.srt.hardware_backend.npu.attention.ascend_backend.AscendAttnBackend" || metadata.at("route") != "eager"
        || metadata.at("batch_size") != 1 || metadata.at("speculative_present") != false || metadata.at("request_table_dtype") != "torch.int32"
        || metadata.at("request_table_contiguous") != true || shape.size() != 2 || dtype != "torch.int64" || metadata.at("sequence_device_type") != "npu"
        || metadata.at("request_indices_dtype") != "torch.int64" || metadata.at("request_indices_device_type") != "npu")
        return nullptr;
    return std::make_unique<InputPreparation>(part, shape.at(0).get<double>(), shape.at(1).get<double>(), "int64", deterministic_sampling);
}

} // namespace

std::unique_ptr<const CallSequence> read_request_table_write(const nlohmann::json & metadata) { return read(metadata, Part::RequestTable); }
std::unique_ptr<const CallSequence> read_forward_inputs(const nlohmann::json & metadata, bool deterministic_sampling) {
    return read(metadata, Part::ForwardInputs, deterministic_sampling);
}

std::optional<bool> read_deterministic_sampling(const std::optional<std::filesystem::path> & configuration) {
    if (!configuration) return std::nullopt;
    std::ifstream stream(*configuration);
    if (!stream) throw std::runtime_error("Cannot read source configuration: " + configuration->string());
    const auto raw = nlohmann::json::parse(stream);
    const auto command = raw.at("server").at("command").get<std::vector<std::string>>();
    const auto entry = std::ranges::find(command, "sglang.launch_server");

    // Shell wrappers do not expose their effective flags as native argv.
    if (entry == command.end() || entry == command.begin() || *std::prev(entry) != "-m") return std::nullopt;
    return std::find(std::next(entry), command.end(), "--enable-deterministic-inference") != command.end();
}
std::unique_ptr<const CallSequence> sampling_positions(const std::string & sequence_dtype) {
    if (sequence_dtype != "torch.int64") return nullptr;
    return std::make_unique<InputPreparation>(Part::SamplingPositions, 0, 0, "int64");
}

} // namespace markov::adapters::sglang
