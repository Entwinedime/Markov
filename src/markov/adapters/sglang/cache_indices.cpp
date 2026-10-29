#include "markov/adapters/sglang/cache_indices.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace markov::adapters::sglang {
namespace {

using namespace trace_graph::planning;

class CacheIndexCopies final : public CallSequence {
public:
    NativeSequence describe(Phase, const FrameInput & input, std::span<const Work> work) const override {
        std::vector<NativeCall> calls;
        for (const auto & copy : work) {
            const bool host = copy.features.at("host") != 0;
            Work clone{
                {                                           "cache.index_clone", host ? "aten.clone:cpu:int64" : "aten.clone:npu:int64", host ? "caller_blocking_interval" : "device_service" },
                { { "elements", copy.features.at("pages") * input.page_tokens },                                 { "element_bytes", 8 },                                 { "tensor_rank", 1 } }
            };
            NativeCall call{ clone, host ? CallKind::HostCall : CallKind::Queued, host ? std::vector<Work>{} : std::vector<Work>{ clone } };
            if (!host) {
                call.queue_name = "aclnnInplaceCopy";
                call.worker_features = {
                    { "element_bytes", 8 },
                    {   "tensor_rank", 1 }
                };
            }
            calls.push_back(std::move(call));
        }
        return { std::move(calls), std::nullopt };
    }
};

class CacheIndexMatch final : public CallSequence {
public:
    explicit CacheIndexMatch(double table_element_bytes) : table_element_bytes_(table_element_bytes) {}
    NativeSequence describe(Phase, const FrameInput & input, std::span<const Work> work) const override {
        std::vector<NativeCall> calls;
        for (const auto & operation : work) {
            const bool concatenate = operation.kind.operation == "cache.index_match";
            auto features = operation.features;
            if (features.contains("pages")) {
                features["elements"] = features.at("pages") * input.page_tokens;
                features.erase("pages");
            }
            features["element_bytes"] = concatenate ? 8 : table_element_bytes_;
            features["tensor_rank"] = 1;
            if (!concatenate) features["source_element_bytes"] = 8;
            Work physical{
                { operation.kind.operation, concatenate ? "aten.cat:npu:int64" : "aten.copy_:npu:int64_to_request_table", "device_service" },
                std::move(features)
            };
            if (concatenate && physical.features.at("elements") == 0) continue;
            const bool queued = physical.features.at("elements") != 0;
            NativeCall call{ physical, queued ? CallKind::Queued : CallKind::Cpu, queued ? std::vector<Work>{ physical } : std::vector<Work>{} };
            if (queued) call.queue_name = concatenate ? "aclnnCat" : "aclnnInplaceCopy";
            calls.push_back(std::move(call));
        }
        return { std::move(calls), std::nullopt };
    }

private:
    double table_element_bytes_;
};

class CacheBackup final : public CallSequence {
public:
    NativeSequence describe(Phase phase, const FrameInput & input, std::span<const Work> work) const override {
        NativeSequence result{ {}, std::nullopt };
        const auto clone = cache_index_copies();
        for (std::size_t id = 0; id < work.size(); ++id) {
            const auto & operation = work[id];
            result.calls.push_back({ operation, CallKind::HostCall, {} });
            result.calls.back().async_work = id;
            const std::vector<Work> indices{
                { { "cache.index_clone", {}, {} }, { { "pages", operation.features.at("pages") }, { "host", 1 } } }
            };
            auto copies = clone->describe(phase, input, indices);
            result.calls.insert(result.calls.end(), std::make_move_iterator(copies.calls.begin()), std::make_move_iterator(copies.calls.end()));
        }
        return result;
    }
};

} // namespace

std::unique_ptr<const trace_graph::planning::CallSequence> cache_index_copies() { return std::make_unique<CacheIndexCopies>(); }

std::unique_ptr<const trace_graph::planning::CallSequence> cache_backup() { return std::make_unique<CacheBackup>(); }

std::unique_ptr<const trace_graph::planning::CallSequence> cache_index_match(double table_element_bytes) {
    return std::make_unique<CacheIndexMatch>(table_element_bytes);
}

std::vector<trace_graph::source::ObservedCallSequence> read_cache_lookup(const trace_graph::source::Observations & observed,
                                                                         const trace_graph::source::ObservedRegion & region) {
    using namespace trace_graph::source;
    const ObservedSpan * first_clone = nullptr;
    const ObservedSpan * last_clone = nullptr;
    const ObservedSpan * concatenation = nullptr;
    for (const auto & span : observed.spans) {
        if (span.resource != region.resource || span.begin_ns < region.begin_ns || region.end_ns < span.end_ns) continue;
        const auto & name = observed.names[span.name];
        if (name == "aten::clone") {
            if (!first_clone || span.begin_ns < first_clone->begin_ns) first_clone = &span;
            if (!last_clone || last_clone->end_ns < span.end_ns) last_clone = &span;
        }
        if (name == "aten::cat") {
            if (concatenation) return {};
            concatenation = &span;
        }
    }
    const auto empty = ObservedRegion{ region.resource, region.end_ns, region.end_ns, region.record };
    const auto copies = first_clone ? ObservedRegion{ region.resource, first_clone->begin_ns, last_clone->end_ns, first_clone->record } : empty;
    if (concatenation && first_clone && concatenation->begin_ns < copies.end_ns) return {};
    std::vector<Work> match;
    for (const auto & control : observed.host_controls) {
        const auto & part = control.region;
        const auto & work = control.work;
        if (work.kind.operation != "cache.match_segments" || part.resource != region.resource || part.begin_ns < region.begin_ns || region.end_ns < part.end_ns
            || work.features.at("elements") == 0)
            continue;
        if (!concatenation || work.features.at("device_npu") != 1 || work.features.at("element_bytes") != 8 || work.features.at("tensor_rank") != 1) return {};
        match.push_back({
            { "cache.index_match", {}, {} },
            { { "elements", work.features.at("elements") }, { "input_tensors", work.features.at("input_tensors") } }
        });
    }
    if (match.size() > 1) match.clear();
    std::vector<ObservedCallSequence> result;
    // Without clone-shape observations, this anchor grants no source clone costs.
    result.push_back({ copies, cache_index_copies(), "cache_query_split" });
    result.push_back({
        concatenation ? ObservedRegion{ region.resource, concatenation->begin_ns, concatenation->end_ns, concatenation->record }
          : empty,
        cache_index_match(8),
        "cache_query_match",
        std::nullopt,
        std::move(match)
    });
    return result;
}

} // namespace markov::adapters::sglang
