#include "markov/adapters/sglang/result_release.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace markov::adapters::sglang {
namespace {

using namespace trace_graph::planning;
using namespace trace_graph::source;

class ReleaseCall final : public CallSequence {
public:
    ReleaseCall(ResultControl point, std::string implementation, double element_bytes)
        : point_(point),
          implementation_(std::move(implementation)),
          element_bytes_(element_bytes) {}

    NativeSequence describe(Phase, const FrameInput & input, std::span<const Work> work) const override {
        const auto & features = work.front().features;
        const auto tokens = features.at("released_tokens");
        const bool append = point_ == ResultControl::UpdateDuplicates || point_ == ResultControl::CommitDuplicates || point_ == ResultControl::ReleaseTail;
        if (point_ == ResultControl::UpdateDuplicates && tokens != 0) return { {}, std::nullopt };
        if (append || (tokens == 0 && point_ == ResultControl::ReleaseGroup)) {
            Work cpu{
                { append ? "allocator.release_pages" : "allocator.finish_release_group", implementation_, "cpu_service" },
                { { "nonempty", tokens > 0 ? 1 : 0 } }
            };
            return { { { std::move(cpu), CallKind::Cpu, {} } }, std::nullopt };
        }
        if (tokens == 0) return { {}, std::nullopt };
        if (point_ == ResultControl::ConvertReleasedIndices) {
            Work conversion{
                { "allocator.release_indices_to_pages",               "aten.cpu_floor_divide_unique_to_npu",                       "cpu_service" },
                {               { "elements", tokens }, { "released_pages", features.at("released_pages") }, { "element_bytes", element_bytes_ } }
            };
            NativeCall call{ std::move(conversion), CallKind::Readback, {} };
            call.synchronization = Work{
                { "runtime.stream_sync", "ascend.runtime", "idle_stream_sync_interval" },
                { { "previous_work_completed", 1 } }
            };
            // Readback and re-upload each synchronize; their host work has no intermediate consumer.
            call.caller_preparations.push_back({ *call.synchronization, "AscendCL@aclrtSynchronizeStream" });
            return { { std::move(call) }, std::nullopt };
        }
        Work concatenate = point_ == ResultControl::ReleaseGroup
            ? Work{ { "allocator.release_group_concatenate", "aten.cat:npu:request_indices", "device_service" },
                    { { "elements", tokens }, { "input_tensors", features.at("input_tensors") }, { "element_bytes", element_bytes_ }, { "tensor_rank", 1 } } }
            : Work{ { "release.concatenate", "ascend.paged_immediate.regular", "device_service" },
                    { { "page_size", input.page_tokens }, { "need_sort", 0 }, { "released_pages", features.at("released_pages") }, { "release_buffer_pages", features.at("free_pages") } } };
        NativeCall call{ concatenate, CallKind::Queued, { concatenate } };
        call.queue_name = "aclnnCat";
        return { { std::move(call) }, std::nullopt };
    }

private:
    ResultControl point_;
    std::string implementation_;
    double element_bytes_;
};

void release_steps(std::vector<ObservedCallSequence> & sequences, const Observations & observed, const HostControl & control, double index_element_bytes,
                   bool immediate) {
    const auto & features = control.work.features;
    const auto tokens = features.at(immediate ? "released_tokens" : "grouped_tokens");
    std::vector<const ObservedSpan *> cats;
    for (const auto & span : observed.spans)
        if (span.resource == control.region.resource && observed.names[span.name] == "aten::cat" && control.region.begin_ns <= span.begin_ns
            && span.end_ns <= control.region.end_ns)
            cats.push_back(&span);
    std::ranges::sort(cats, {}, &ObservedSpan::begin_ns);
    const bool nonempty = tokens != 0;
    if (cats.size() != (nonempty ? immediate ? 1 : 2 : 0)) return;
    const std::vector<Work> source_work{
        { { "allocator.release_group", {}, {} },
         { { "released_tokens", tokens },
            { "input_tensors", immediate ? nonempty ? 1.0 : 0.0 : features.at("grouped_tensors") },
            { "free_pages", features.at("free_pages") },
            { "released_pages", features.at("free_pages_after") - features.at("free_pages") } } }
    };
    const auto first_end = !nonempty ? control.region.end_ns : immediate ? control.region.begin_ns : cats.front()->end_ns;
    const auto last_begin = nonempty ? cats.back()->begin_ns : control.region.end_ns;
    const auto entry = immediate ? ResultControl::UpdateDuplicates : ResultControl::ReleaseGroup;
    sequences.push_back({
        { control.region.resource, control.region.begin_ns, first_end, control.region.record },
        std::make_unique<ReleaseCall>(entry, control.work.kind.implementation, index_element_bytes),
        immediate ? "result_release_append" : "result_release_group",
        entry,
        source_work
    });
    sequences.push_back({
        { control.region.resource, first_end, last_begin, control.region.record },
        std::make_unique<ReleaseCall>(ResultControl::ConvertReleasedIndices, control.work.kind.implementation, index_element_bytes),
        "result_release_conversion",
        ResultControl::ConvertReleasedIndices,
        source_work
    });
    sequences.push_back({
        { control.region.resource, last_begin, control.region.end_ns, control.region.record },
        std::make_unique<ReleaseCall>(ResultControl::PublishReleasedPages, control.work.kind.implementation, index_element_bytes),
        "result_release_publish",
        ResultControl::PublishReleasedPages,
        source_work
    });
}

} // namespace

std::vector<ObservedCallSequence> read_result_releases(const Observations & observed, const BatchResult & result, double index_element_bytes) {
    std::vector<ObservedCallSequence> sequences;
    for (const auto id : result.processed->controls) {
        const auto & control = observed.host_controls[id];
        const auto & features = control.work.features;
        if (control.work.kind.operation == "allocator.release_pages") {
            const HostControl * parent = nullptr;
            for (const auto owner : result.processed->controls) {
                const auto & candidate = observed.host_controls[owner];
                if ((candidate.work.kind.operation == "cache.update_prefix" || candidate.work.kind.operation == "cache.commit_prefix")
                    && candidate.region.begin_ns <= control.region.begin_ns && control.region.end_ns <= candidate.region.end_ns)
                    parent = &candidate;
            }
            if (!parent) continue;
            if (parent->work.kind.operation == "cache.update_prefix" && features.at("grouped") == 0) {
                release_steps(sequences, observed, control, index_element_bytes, true);
                continue;
            }
            if (features.at("grouped") != 1) continue;
            auto point = ResultControl::UpdateDuplicates;
            if (parent->work.kind.operation == "cache.commit_prefix") {
                const bool earlier_release = std::ranges::any_of(result.processed->controls, [&](Index other) {
                    const auto & previous = observed.host_controls[other];
                    return previous.work.kind.operation == "allocator.release_pages" && previous.work.features.at("grouped") == 1
                           && parent->region.begin_ns <= previous.region.begin_ns && previous.region.end_ns <= control.region.begin_ns;
                });
                point = earlier_release ? ResultControl::ReleaseTail : ResultControl::CommitDuplicates;
            }
            sequences.push_back({ control.region,
                                  std::make_unique<ReleaseCall>(point, control.work.kind.implementation, index_element_bytes),
                                  "result_release_append",
                                  point,
                                  { { { "allocator.release_indices", {}, {} }, { { "released_tokens", features.at("released_tokens") } } } } });
        }
        if (control.work.kind.operation != "allocator.finish_release_group") continue;
        if (features.at("need_sort") != 0) continue;
        release_steps(sequences, observed, control, index_element_bytes, false);
    }
    return sequences;
}

} // namespace markov::adapters::sglang
