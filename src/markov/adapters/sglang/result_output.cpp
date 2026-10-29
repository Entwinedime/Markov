#include "markov/adapters/sglang/result_output.hpp"

#include <algorithm>
#include <cmath>
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

class ResultCaller final : public CallSequence {
public:
    ResultCaller(Features features, bool send) : features_(std::move(features)), send_(send) {}
    NativeSequence describe(Phase phase, const FrameInput & input, std::span<const Work>) const override {
        auto features = features_;
        features["batch_size"] = input.batch_size;
        const std::string operation = std::string(phase == Phase::Prefill ? "prefill.result." : "decode.result.") + (send_ ? "send" : "caller");
        Work work{
            { operation, send_ ? "zmq.Socket.send_pyobj:BatchTokenIDOutput" : "sglang.scheduler.process_batch_result", "cpu_service" },
            std::move(features)
        };
        NativeCall call{ std::move(work), CallKind::Cpu, {} };
        if (features_.at("entry") == 1)
            call.caller_preparations.push_back({
                { { "device.event.synchronize", "ascend.aclrtSynchronizeEvent.ready", "cpu_service" }, { { "operations", 1 } } },
                "AscendCL@aclrtSynchronizeEvent"
            });
        return { { std::move(call) }, std::nullopt, send_ ? std::optional(NativeProducer::Caller) : std::nullopt };
    }

private:
    Features features_;
    bool send_;
};

class EmptyRelease final : public CallSequence {
public:
    explicit EmptyRelease(const trace_graph::source::HostControl & control) : work_(control.work) {
        // Empty returns never inspect page geometry or the free-page arrays.
        work_.kind.timing_scope = "cpu_service";
        work_.features = {};
    }
    NativeSequence describe(Phase, const FrameInput &, std::span<const Work>) const override { return { { { work_, CallKind::Cpu, {} } }, std::nullopt }; }

private:
    Work work_;
};

class CacheIndexValues final : public CallSequence {
public:
    CacheIndexValues(double tokens, double source_element_bytes) : tokens_(tokens), source_element_bytes_(source_element_bytes) {}
    NativeSequence describe(Phase, const FrameInput & input, std::span<const Work>) const override {
        if (input.page_tokens <= 0) return { {}, "cache_index_copy_needs_page_geometry" };
        const auto elements = std::floor(tokens_ / input.page_tokens) * input.page_tokens;
        if (elements == 0) return { {}, "empty_cache_index_copy_requires_a_native_rule" };

        Work work{
            { "cache.index_values", "aten._to_copy:npu:int32_or_int64_to_int64", "device_service" },
            { { "elements", elements }, { "source_element_bytes", source_element_bytes_ }, { "element_bytes", 8 }, { "tensor_rank", 1 } }
        };
        NativeCall call{ work, CallKind::Queued, { work } };
        call.queue_name = "aclnnInplaceCopy";
        call.worker_features = {
            { "source_element_bytes", source_element_bytes_ },
            {        "element_bytes",                     8 },
            {          "tensor_rank",                     1 }
        };
        call.caller_work = Work{
            { "cache.index_values", "aten._to_copy:npu:int32_or_int64_to_int64", "cpu_service" },
            *call.worker_features
        };
        call.caller_preparations.push_back({
            { { "runtime.tensor.allocate", "aten.empty:npu:int32_or_int64", "caller_blocking_interval" },
             { { "bytes", elements * 8 }, { "element_bytes", 8 }, { "tensor_rank", 1 } } },
            "aten::empty"
        });
        return { { std::move(call) }, std::nullopt };
    }

private:
    double tokens_, source_element_bytes_;
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

std::vector<trace_graph::source::ObservedCallSequence> read_result_callers(const trace_graph::source::Observations & observed,
                                                                           const trace_graph::source::BatchResult & result,
                                                                           const nlohmann::json * input_metadata) {
    using namespace trace_graph::source;
    if (!result.processed || !result.copy || result.copy->additional_outputs || result.copy->token_shape != std::vector<Index>{ 1 }) return {};
    const auto & processed = *result.processed;
    if (processed.output_tokens_before.size() != 1) return {};
    const auto & parent = processed.region;
    std::vector<Time> cuts{ parent.begin_ns, parent.end_ns };
    for (const auto id : processed.controls) {
        const auto & control = observed.host_controls[id].region;
        cuts.insert(cuts.end(), { control.begin_ns, control.end_ns });
    }
    for (const auto & send : result.final_sends) cuts.insert(cuts.end(), { send.region.begin_ns, send.region.end_ns });
    std::ranges::sort(cuts);
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    std::vector<ObservedCallSequence> callers;
    for (Index at = 1; at < cuts.size(); ++at) {
        const auto begin = cuts[at - 1], end = cuts[at];
        const HostControl * owner = nullptr;
        for (const auto id : processed.controls) {
            const auto & control = observed.host_controls[id];
            if (control.region.begin_ns <= begin && end <= control.region.end_ns
                && (!owner || control.region.end_ns - control.region.begin_ns < owner->region.end_ns - owner->region.begin_ns))
                owner = &control;
        }
        if (owner) {
            const auto & work = owner->work;
            const bool empty_free = work.kind.operation == "allocator.release_pages" && work.features.at("released_tokens") == 0;
            const bool empty_group = work.kind.operation == "allocator.finish_release_group" && work.features.at("grouped_tensors") == 0;
            if (!empty_free && !empty_group) continue;
        }

        // A source caller fragment cannot conceal a missing native lifecycle.
        for (const auto & call : observed.native_queue_calls)
            if (call.submission.resource == parent.resource && call.submission.begin_ns < end && begin < call.submission.end_ns) return {};
        for (const auto & call : observed.native_runtime_calls)
            if (call.device_submission && call.region.resource == parent.resource && call.region.begin_ns < end && begin < call.region.end_ns) return {};

        if (owner) {
            callers.push_back({
                { parent.resource, begin, end, parent.record },
                std::make_unique<EmptyRelease>(*owner),
                "result_empty_release"
            });
            continue;
        }
        const bool send = std::ranges::any_of(result.final_sends, [&](const ResultSend & value) {
            return value.region.resource == parent.resource && value.region.begin_ns <= begin && end <= value.region.end_ns;
        });
        const bool after_send = std::ranges::any_of(result.final_sends, [&](const ResultSend & value) { return value.region.end_ns <= begin; });
        Features features{
            { "output_tokens_before",    processed.output_tokens_before.front() },
            {  "output_tokens_after",     processed.output_tokens_after.front() },
            {      "finished_before", processed.finished_before.front() ? 1 : 0 },
            {       "finished_after",  processed.finished_after.front() ? 1 : 0 },
            {                "entry",          begin == parent.begin_ns ? 1 : 0 },
            {                 "exit",              end == parent.end_ns ? 1 : 0 },
            {           "after_send",                        after_send ? 1 : 0 }
        };
        callers.push_back({
            { parent.resource, begin, end, parent.record },
            std::make_unique<ResultCaller>(std::move(features), send),
            send ? "result_send" : "result_caller"
        });
    }
    if (input_metadata && input_metadata->value("request_table_contiguous", false)) {
        const auto dtype = input_metadata->at("request_table_dtype").get<std::string>();
        if (dtype == "torch.int32" || dtype == "torch.int64") {
            for (const auto id : processed.controls) {
                const auto & control = observed.host_controls[id];
                const auto & operation = control.work.kind.operation;
                if (operation != "cache.update_prefix" && operation != "cache.commit_prefix") continue;
                const auto tokens = control.work.features.at(operation == "cache.update_prefix" ? "input_tokens" : "committed_tokens");
                const NativeQueueCall * first = nullptr;
                for (const auto & call : observed.native_queue_calls)
                    if (call.submission.resource == parent.resource && control.region.begin_ns <= call.submission.begin_ns
                        && call.submission.end_ns <= control.region.end_ns && (!first || call.submission.begin_ns < first->submission.begin_ns))
                        first = &call;
                if (!first || observed.names[first->name] != "aclnnInplaceCopy") continue;
                const ObservedSpan * copy = nullptr;
                for (const auto & span : observed.spans) {
                    if (span.resource != parent.resource || observed.names[span.name] != "aten::to" || span.begin_ns < control.region.begin_ns
                        || control.region.end_ns < span.end_ns || first->submission.begin_ns < span.begin_ns || span.end_ns < first->submission.end_ns)
                        continue;
                    if (copy) return {};
                    copy = &span;
                }
                if (!copy) continue;
                callers.push_back({
                    { copy->resource, copy->begin_ns, copy->end_ns, copy->record },
                    std::make_unique<CacheIndexValues>(tokens, dtype == "torch.int32" ? 4 : 8),
                    "cache_index_values"
                });
            }
        }
    }
    std::ranges::sort(callers, {}, [](const ObservedCallSequence & value) { return value.region.begin_ns; });
    for (Index id = 1; id < callers.size(); ++id)
        if (callers[id - 1].region.end_ns > callers[id].region.begin_ns) return {};
    return callers;
}

} // namespace markov::adapters::sglang
