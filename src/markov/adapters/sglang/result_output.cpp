#include "markov/adapters/sglang/result_output.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace markov::adapters::sglang {
namespace {

using namespace trace_graph::planning;

void queued(std::vector<NativeCall> & calls, const std::string & operation, const char * implementation, const char * queue, const Features & features) {
    Work work{
        { operation, implementation, "device_service" },
        features
    };
    calls.push_back({ work, CallKind::Queued, { work } });
    calls.back().queue_name = queue;
}

void relay_write(std::vector<NativeCall> & calls, const std::string & operation, const char * implementation, const Features & indexed,
                 const Features & values) {
    queued(calls, operation, implementation, "aclnnIndexPutImpl", indexed);
    auto broadcast = calls.back().work;
    broadcast.kind.operation += ".broadcast";
    broadcast.kind.implementation = "ascend.broadcast_to.int64.aicore";
    broadcast.features = values;
    auto update = calls.back().work;
    update.kind.implementation = "ascend.index_put_v2.int64.aicore";
    calls.back().launches = { std::move(broadcast), std::move(update) };
}

class ResultOutput final : public CallSequence {
public:
    explicit ResultOutput(double pool_rows) : pool_rows_(pool_rows) {}
    NativeSequence describe(Phase phase, const FrameInput & input, std::span<const Work>) const override;

private:
    double pool_rows_;
};

NativeSequence ResultOutput::describe(Phase phase, const FrameInput & input, std::span<const Work>) const {
    const std::string prefix = phase == Phase::Prefill ? "prefill.output." : "decode.output.";
    const Features tensor{
        {    "batch_size", input.batch_size },
        {      "elements", input.batch_size },
        { "element_bytes",                8 }
    };
    auto indexed = tensor;
    indexed["buffer_rows"] = pool_rows_;
    const Features transfer{
        {        "bytes", input.batch_size * 8 },
        { "non_blocking",                    1 }
    };
    const Features marker{
        { "operations", 1 }
    };
    std::vector<NativeCall> calls;
    // Non-speculative FutureMap publishes lengths and sampled tokens to the
    // request-pool relay consumed by the following iteration.
    queued(calls, prefix + "advance_lengths", "aten.add:int64:scalar=1", "aclnnAdds", tensor);
    relay_write(calls, prefix + "publish_lengths", "aten.index_put_:future_lengths:int64:int64_indices", indexed, tensor);
    relay_write(calls, prefix + "publish_tokens", "aten.index_put_:future_tokens:int64:int64_indices", indexed, tensor);
    queued(calls, prefix + "copy_to_cpu", "ascend.memcpy.device_to_host:int64", "acl_memcpy_device_to_host", transfer);
    queued(calls, prefix + "copy_done", "ascend.event_record", "record_event", marker);
    calls.back().device_control = DeviceControl::Record;
    for (std::size_t id = 0; id < calls.size(); ++id) {
        auto caller = calls[id].work;
        caller.kind.implementation = "sglang.batch_output";
        caller.kind.timing_scope = "cpu_service";
        caller.features["sequence_entry"] = id == 0 ? 1 : 0;
        caller.features["sequence_exit"] = id + 1 == calls.size() ? 1 : 0;
        calls[id].caller_work = std::move(caller);
    }
    return { std::move(calls), std::nullopt, NativeProducer::Device };
}

} // namespace

std::unique_ptr<const trace_graph::planning::CallSequence> read_result_output(const nlohmann::json & metadata, const trace_graph::source::ResultCopy & copy) {
    if (metadata.at("route") != "eager" || metadata.at("speculative_present") != false || metadata.at("batch_size") != 1
        || metadata.at("sequence_dtype") != "torch.int64" || metadata.at("request_indices_dtype") != "torch.int64"
        || metadata.at("request_indices_device_type") != "npu" || copy.additional_outputs || copy.token_device != "npu" || copy.token_dtype != "torch.int64"
        || copy.token_shape != std::vector<trace_graph::source::Index>{ 1 } || copy.token_bytes != 8)
        return nullptr;

    return std::make_unique<ResultOutput>(metadata.at("request_table_shape").at(0).get<double>());
}

} // namespace markov::adapters::sglang
