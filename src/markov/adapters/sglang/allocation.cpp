#include "markov/adapters/sglang/allocation.hpp"
#include "markov/adapters/sglang/kernel_preparation.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace markov::adapters::sglang {
namespace {

using namespace trace_graph::planning;

class DecodeAllocation final : public CallSequence {
public:
    NativeSequence describe(Phase, const FrameInput & input, std::span<const Work>) const override;
};

class ExtendAllocation final : public CallSequence {
public:
    NativeSequence describe(Phase, const FrameInput & input, std::span<const Work>) const override;
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

NativeSequence DecodeAllocation::describe(Phase, const FrameInput & input, std::span<const Work>) const {
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
    return { std::move(calls), std::nullopt };
}

NativeSequence ExtendAllocation::describe(Phase, const FrameInput & input, std::span<const Work>) const {
    if (!input.device_pages_allocated_since_return) return { {}, "allocator_index_view_unavailable" };
    const auto prefix_tokens = input.context_tokens - input.query_tokens;
    const auto pages = std::ceil(input.context_tokens / input.page_tokens) - std::ceil(prefix_tokens / input.page_tokens);
    if (input.query_tokens <= 0 || pages >= 200) return { {}, "non_fused_extend_allocator_unavailable" };

    const Features shape{
        {       "batch_size",  input.batch_size },
        {         "elements",  input.batch_size },
        { "page_size_tokens", input.page_tokens }
    };
    const std::string prefix = "allocator.extend.", caller = "sglang.NPUPagedTokenToKVPoolAllocator";
    std::vector<NativeCall> calls;
    const std::pair<const char *, const char *> counts[]{
        { "sequence_roundup",          "aten.add:int64" },
        {   "sequence_pages", "aten.floor_divide:int64" },
        {   "prefix_roundup",          "aten.add:int64" },
        {     "prefix_pages", "aten.floor_divide:int64" },
        {        "new_pages",          "aten.sub:int64" },
        {        "sum_pages",          "aten.sum:int64" }
    };
    for (const auto & [name, implementation] : counts) {
        calls.push_back({
            { { calls.empty() ? prefix + "entry" : prefix + name + ".prepare", caller, "cpu_service" }, shape },
            CallKind::Cpu,
            {}
        });
        Work work{
            { prefix + name, implementation, "device_service" },
            shape
        };
        auto launch = work;
        if (work.kind.implementation == "aten.floor_divide:int64") {
            launch.kind.operation += ".divide";
            launch.kind.implementation = "ascend.floor_divide.int64.aicore";
        }
        calls.push_back({ std::move(work), CallKind::Queued, { std::move(launch) } });
    }
    calls.push_back({
        { { prefix + "page_count_readback.prepare", caller, "cpu_service" }, shape },
        CallKind::Cpu,
        {}
    });
    NativeCall readback{
        { { prefix + "page_count_readback", "aten._local_scalar_dense:int64", "cpu_service" }, shape },
        CallKind::Readback,
        {}
    };
    readback.synchronization = Work{
        { "runtime.stream_sync", "aclrtSynchronizeStreamWithTimeout", "idle_stream_sync_interval" },
        { { "previous_work_completed", 1 }, { "timeout_ms", -1 } }
    };
    calls.push_back(std::move(readback));

    // The native fused/naive switch uses newly allocated pages, not total context.
    auto kernel_shape = shape;
    kernel_shape["elements"] = input.query_tokens;
    kernel_shape["triton_task_queue_enabled"] = 1;
    kernel_shape["batch_upper_bound"] = 1;
    kernel_shape["extend_upper_bound_tokens"] = std::bit_ceil(static_cast<std::size_t>(input.query_tokens));
    kernel_shape["free_pointer_mod16_bytes"] = (*input.device_pages_allocated_since_return % 2) * 8;
    Features specialization = kernel_shape;
    specialization.erase("elements");
    specialization.erase("batch_size");
    specialization.erase("triton_task_queue_enabled");
    Work kernel{
        { prefix + "fused_indices", "sgl_kernel_npu.alloc_extend_kernel:int64", "device_service" },
        kernel_shape
    };
    calls.push_back({
        { { prefix + "fused_indices.prepare", caller, "cpu_service" }, kernel_shape },
        CallKind::Cpu,
        {}
    });
    NativeCall launch{ kernel, CallKind::Queued, { kernel } };
    launch.preparation = triton_preparation("alloc_extend",
                                            std::move(specialization),
                                            {
                                                {     "pointer_arguments", 5 },
                                                { "pointer_element_bytes", 8 },
                                                {    "constant_arguments", 4 }
    });
    calls.push_back(std::move(launch));
    Work cast{
        { prefix + "output_int", "aten._to_copy:int64_to_int32", "device_service" },
        shape
    };
    // Output conversion covers the extension vector, not the batch-length vector.
    cast.features["elements"] = input.query_tokens;
    calls.push_back({
        { { prefix + "output_int.prepare", caller, "cpu_service" }, cast.features },
        CallKind::Cpu,
        {}
    });
    calls.push_back({ cast, CallKind::Queued, { cast } });
    calls.push_back({
        { { prefix + "return", caller, "cpu_service" }, shape },
        CallKind::Cpu,
        {}
    });
    return { std::move(calls), std::nullopt };
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

std::unique_ptr<const CallSequence> read_extend_allocator(const nlohmann::json & args) {
    if (args.at("allocator_implementation") != "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator"
        || !args.at("device_name").get<std::string>().starts_with("Ascend910B") || args.at("batch_size") != 1 || args.at("sequence_dtype") != "torch.int64"
        || args.at("prefix_dtype") != "torch.int64" || args.at("last_location_dtype") != "torch.int64" || args.at("free_page_dtype") != "torch.int64"
        || args.at("free_page_stride_elements") != 1 || args.at("debug_memory_pool") != false || args.at("allocator_need_sort") != false
        || args.at("status") != "returned" || args.at("allocated") != true || args.at("triton_task_queue_enabled") != true
        || !args.at("supplied_new_pages").is_null())
        return nullptr;
    return std::make_unique<ExtendAllocation>();
}

} // namespace markov::adapters::sglang
