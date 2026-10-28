#include "markov/adapters/sglang/metadata.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace markov::adapters::sglang {
namespace {

using namespace trace_graph::planning;

class Metadata final : public CallSequence {
public:
    Metadata(double capacity, double rows, double columns, std::string sequence_dtype, std::optional<std::string> extend_dtype)
        : capacity_(capacity),
          rows_(rows),
          columns_(columns),
          sequence_dtype_(std::move(sequence_dtype)),
          extend_dtype_(std::move(extend_dtype)) {}
    std::vector<NativeCall> describe(Phase phase, const AttentionInput & input) const override;

private:
    double capacity_, rows_, columns_;
    std::string sequence_dtype_;
    std::optional<std::string> extend_dtype_;
};

void append(std::vector<NativeCall> & calls, const std::string & prefix, const Features & shape, NativeCall call) {
    const auto preparation = calls.empty() ? prefix + "entry" : call.work.kind.operation + ".prepare";
    calls.push_back({
        { { preparation, "sglang.AscendAttnBackend.metadata.eager", "cpu_service" }, shape },
        CallKind::Cpu,
        {}
    });
    if (call.kind == CallKind::Readback)
        call.synchronization = Work{
            { "runtime.stream_sync", "aclrtSynchronizeStreamWithTimeout", "idle_stream_sync_interval" },
            { { "previous_work_completed", 1 }, { "timeout_ms", -1 } }
        };
    calls.push_back(std::move(call));
}

std::vector<NativeCall> Metadata::describe(Phase phase, const AttentionInput & input) const {
    const std::string prefix = phase == Phase::Prefill ? "prefill.metadata." : "decode.metadata.";
    const auto pages = std::ceil(input.context_tokens / input.page_tokens);
    const Features shape{
        {                  "batch_size",                         input.batch_size },
        {              "context_tokens",                     input.context_tokens },
        {            "page_size_tokens",                        input.page_tokens },
        {          "page_table_columns",                                    pages },
        { "page_table_capacity_columns", std::ceil(capacity_ / input.page_tokens) }
    };
    const std::string implementation = "sglang.AscendAttnBackend.metadata.eager";
    std::vector<NativeCall> calls;
    auto scalar = shape;
    scalar["elements"] = 1;
    Work maximum{
        { prefix + "context_max", "aten.max:" + sequence_dtype_, "device_service" },
        scalar
    };
    append(calls, prefix, shape, { maximum, CallKind::Queued, { maximum } });
    append(calls,
           prefix,
           shape,
           {
               { { prefix + "context_readback", "aten._local_scalar_dense:" + sequence_dtype_, "cpu_service" }, scalar },
               CallKind::Readback,
               {}
    });

    auto indices = shape;
    indices["elements"] = input.context_tokens;
    indices["table_rows"] = rows_;
    indices["table_columns"] = columns_;
    std::vector<Work> launches;
    // Slicing all table rows retains the original storage stride before index.
    if (rows_ > 1 && input.context_tokens < columns_) {
        auto materialized = indices;
        materialized["elements"] *= rows_;
        launches.push_back({
            { prefix + "token_indices.materialize", "ascend.slice.int32.aicore", "device_service" },
            std::move(materialized)
        });
    }
    launches.push_back({
        { prefix + "token_indices.gather", "ascend.index.int32.aicore", "device_service" },
        indices
    });
    append(calls,
           prefix,
           shape,
           {
               { { prefix + "token_indices", "aten.index:page_table_int32", "device_service" }, std::move(indices) },
               CallKind::Queued,
               std::move(launches)
    });

    const Features division{
        {       "batch_size",     input.batch_size },
        {         "elements",                pages },
        {   "input_elements", input.context_tokens },
        { "page_size_tokens",    input.page_tokens }
    };
    launches.clear();
    // A partial final page exceeds CANN Slice's reshaped storage bounds;
    // AsStrided handles it. A singleton view requires no materialization.
    if (input.page_tokens > 1 && pages > 1) {
        const auto native = std::fmod(input.context_tokens, input.page_tokens) == 0 ? "ascend.slice.int32.aicore" : "ascend.as_strided.int32.aicore";
        launches.push_back({
            { prefix + "page_division.materialize", native, "device_service" },
            division
        });
    }
    launches.push_back({
        { prefix + "page_division.promote", "ascend.cast.int32_to_int64", "device_service" },
        division
    });
    launches.push_back({
        { prefix + "page_division.divide", "ascend.floor_divide.int64.aicore", "device_service" },
        division
    });
    launches.push_back({
        { prefix + "page_division.output", "ascend.cast.int64_to_int32", "device_service" },
        division
    });
    append(calls,
           prefix,
           shape,
           {
               { { prefix + "page_division", "aten.floor_divide:page_table_int32", "device_service" }, division },
               CallKind::Queued,
               std::move(launches)
    });

    if (extend_dtype_)
        append(calls,
               prefix,
               shape,
               {
                   { { prefix + "extend_readback", "aten.to.cpu:" + *extend_dtype_, "cpu_service" }, scalar },
                   CallKind::Readback,
                   {}
        });
    if (sequence_dtype_ != "torch.int32") {
        Work cast{
            { prefix + "sequence_cast", "aten._to_copy:" + sequence_dtype_ + "->torch.int32", "device_service" },
            scalar
        };
        append(calls, prefix, shape, { cast, CallKind::Queued, { cast } });
    }
    calls.push_back({
        { { prefix + "return", implementation, "cpu_service" }, shape },
        CallKind::Cpu,
        {}
    });
    return calls;
}

} // namespace

std::unique_ptr<const CallSequence> read_metadata(const nlohmann::json & args) {
    if (args.at("backend_implementation") != "sglang.srt.hardware_backend.npu.attention.ascend_backend.AscendAttnBackend" || args.at("route") != "eager"
        || !args.at("device_name").get<std::string>().starts_with("Ascend910B") || args.at("batch_size") != 1 || args.at("in_capture") != false
        || args.at("speculative_present") != false || args.at("mode_is_target_verify") != false || args.at("mode_is_draft_extend") != false
        || args.at("hybrid_swa") != false || args.at("sliding_window_kv_pool") != false || args.at("use_mla") != false
        || args.at("request_table_dtype") != "torch.int32" || args.at("request_table_contiguous") != true || args.at("sequence_device_type") != "npu"
        || args.at("request_indices_device_type") != "npu" || args.at("request_indices_dtype") != "torch.int64"
        || args.at("sequence_shape") != nlohmann::json::array({ 1 }) || args.at("request_indices_shape") != nlohmann::json::array({ 1 }))
        return nullptr;

    const auto dtype = args.at("sequence_dtype").get<std::string>();
    const auto table = args.at("request_table_shape").get<std::vector<double>>();
    const auto context = args.at("context_tokens").get<double>(), capacity = args.at("max_context_tokens").get<double>();
    if ((dtype != "torch.int32" && dtype != "torch.int64") || table.size() != 2 || table[0] < 1 || table[1] < context || context < 1 || capacity < context)
        return nullptr;
    std::optional<std::string> extend;
    if (args.at("extend_present") == true) {
        if (args.at("extend_device_type") != "npu" && args.at("extend_device_type") != "cpu") return nullptr;
        if (args.at("extend_device_type") == "npu") extend = args.at("extend_dtype").get<std::string>();
    }
    if (extend && *extend != "torch.int32" && *extend != "torch.int64") return nullptr;
    return std::make_unique<Metadata>(capacity, table[0], table[1], dtype, std::move(extend));
}

} // namespace markov::adapters::sglang
