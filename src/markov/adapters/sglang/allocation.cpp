#include "markov/adapters/sglang/allocation.hpp"

#include <cmath>
#include <string>
#include <utility>

namespace markov::adapters::sglang {
namespace {

using namespace trace_graph::planning;

class DecodeAllocation final : public CallSequence {
public:
    std::vector<NativeCall> describe(Phase, const AttentionInput & input, std::span<const Work>) const override;
};

void append(std::vector<NativeCall> & calls, const Features & shape, const std::string & operation, const std::string & implementation,
            std::vector<WorkKind> launches = {}) {
    const std::string prefix = "allocator.decode.", caller = "sglang.NPUPagedTokenToKVPoolAllocator";
    calls.push_back({
        { { calls.empty() ? prefix + "entry" : prefix + operation + ".prepare", caller, "cpu_service" }, shape },
        CallKind::Cpu,
        {}
    });
    Work work{
        { prefix + operation, implementation, "device_service" },
        shape
    };
    NativeCall call{ work, CallKind::Queued, {} };
    if (launches.empty()) call.launches.push_back(work);
    else
        for (auto & kind : launches) call.launches.push_back({ std::move(kind), shape });
    calls.push_back(std::move(call));
}

std::vector<NativeCall> DecodeAllocation::describe(Phase, const AttentionInput & input, std::span<const Work>) const {
    const Features shape{
        {       "batch_size",  input.batch_size },
        {         "elements",  input.batch_size },
        { "page_size_tokens", input.page_tokens }
    };
    const std::string prefix = "allocator.decode.", caller = "sglang.NPUPagedTokenToKVPoolAllocator";
    std::vector<NativeCall> calls;

    // CANN broadcasts the scalar divisor; both launches share one task.
    append(calls,
           shape,
           "remainder",
           "aten.remainder:int64",
           {
               { prefix + "remainder.broadcast", "ascend.broadcast.int64.aicore", "device_service" },
               {    prefix + "remainder.modulo", "ascend.floor_mod.int64.aicore", "device_service" }
    });
    append(calls, shape, "needs_page", "aten.eq:int64");
    append(calls, shape, "needs_page_int", "aten._to_copy:bool_to_int32");
    append(calls,
           shape,
           "end_pages",
           "aten.cumsum:int32_to_int64",
           {
               { prefix + "end_pages.cast", "ascend.cast.int32_to_int64", "device_service" },
               { prefix + "end_pages.scan",  "ascend.cumsum.int64.aicpu", "device_service" }
    });
    append(calls,
           shape,
           "start_pages",
           "aten.sub:int64_int32",
           {
               {    prefix + "start_pages.cast", "ascend.cast.int32_to_int64", "device_service" },
               { prefix + "start_pages.compute",    "ascend.sub.int64.aicore", "device_service" }
    });
    append(calls, shape, "next_location", "aten.add:int32");

    // The native allocator still computes the prefix scan on a page hit.
    // Only the location-selection branch depends on crossing a page boundary.
    if (std::fmod(input.context_tokens, input.page_tokens) == 1) {
        append(calls, shape, "keep_page", "aten.rsub:int32");
        append(calls, shape, "existing_location", "aten.mul:int32");
        append(calls, shape, "new_pages", "aten.index:int64");
        append(calls, shape, "page_offsets", "aten.mul:int64");
        append(calls,
               shape,
               "new_location",
               "aten.mul:int64_int32",
               {
                   {    prefix + "new_location.cast", "ascend.cast.int32_to_int64", "device_service" },
                   { prefix + "new_location.compute",    "ascend.mul.int64.aicore", "device_service" }
        });
        append(calls,
               shape,
               "output_location",
               "aten.add:int32_int64",
               {
                   {    prefix + "output_location.cast", "ascend.cast.int32_to_int64", "device_service" },
                   { prefix + "output_location.compute",    "ascend.add.int64.aicore", "device_service" }
        });
        append(calls, shape, "output_int", "aten._to_copy:int64_to_int32");
    }
    calls.push_back({
        { { prefix + "return", caller, "cpu_service" }, shape },
        CallKind::Cpu,
        {}
    });
    return calls;
}

} // namespace

std::unique_ptr<const CallSequence> read_decode_allocator(const nlohmann::json & args) {
    if (args.at("allocator_implementation") != "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator"
        || !args.at("device_name").get<std::string>().starts_with("Ascend910B") || args.at("batch_size") != 1 || args.at("sequence_dtype") != "torch.int64"
        || args.at("last_location_dtype") != "torch.int32" || args.at("free_page_dtype") != "torch.int64" || args.at("free_page_stride_elements") != 1
        || args.at("debug_memory_pool") != false || args.at("allocator_need_sort") != false || args.at("status") != "returned" || args.at("allocated") != true)
        return nullptr;

    return std::make_unique<DecodeAllocation>();
}

} // namespace markov::adapters::sglang
