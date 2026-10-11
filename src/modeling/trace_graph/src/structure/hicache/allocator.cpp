#include "markov/trace_graph/structure/hicache/allocator.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <bit>
#include <utility>

namespace markov::trace_graph::structure::hicache {
namespace {

// NPUPagedTokenToKVPoolAllocator.alloc_extend uses the naive tensor program at
// this page count. This is an execution branch, not a fitted threshold.
constexpr uint64_t kNpuNaiveAllocationMinimumPages = 200;

class AllocatorProgram {
public:
    AllocatorProgram(const modules::hicache::model::HiCacheAllocatorWorkItem & work, std::string phase)
        : call_(work),
          component_(std::move(phase)),
          prefix_("allocator." + (component_ == "prefill" ? std::string("extend") : component_)) {}

    void adopt_prefix(std::vector<TensorOperation> prefix) {
        program = std::move(prefix);
        for (size_t i = 0; i < program.size(); ++i) {
            const auto kind = program[i].execution;
            if (kind != TensorExecutionKind::CpuService) previous_device_ = i;
            if (kind != TensorExecutionKind::AsyncDevice) previous_host_ = i;
        }
    }

    size_t append(const std::string & operation, const std::string & implementation, uint64_t elements, std::vector<size_t> dependencies,
                  TensorExecutionKind execution = TensorExecutionKind::AsyncDevice) {
        const bool device = execution != TensorExecutionKind::CpuService;
        const std::map<std::string, double> features{
            {         "elements",         static_cast<double>(elements) },
            {       "batch_size", static_cast<double>(call_.batch_size) },
            { "page_size_tokens",  static_cast<double>(call_.page_size) },
        };
        if (device) {
            const auto preparation = program.size();
            std::vector<size_t> host_dependencies;
            if (previous_host_) host_dependencies.push_back(*previous_host_);
            program.push_back({
                .work = { component_,
                 preparation == 0 ? prefix_ + ".entry" : prefix_ + "." + operation + ".prepare",
                 "sglang.NPUPagedTokenToKVPoolAllocator", "cpu_service",
                 features },
                .predecessors = std::move(host_dependencies),
                .execution = TensorExecutionKind::CpuService
            });
            previous_host_ = preparation;
            dependencies.push_back(preparation);
            if (previous_device_ && std::ranges::find(dependencies, *previous_device_) == dependencies.end()) dependencies.push_back(*previous_device_);
        }
        else if (previous_host_) dependencies.push_back(*previous_host_);

        program.push_back({
            .work = { component_, prefix_ + "." + operation, implementation, device ? "device_service" : "cpu_service", features },
            .predecessors = std::move(dependencies),
            .execution = execution
        });
        if (implementation == "aten.cumsum:int32_to_int64" || implementation == "aten.cumsum:int64") {
            // The inspected contiguous integer CANN path casts to the output
            // dtype before scanning. On 910B int64 scan uses AICPU. These are
            // launches in the same queue task; there is no second host submit.
            auto & call = program.back();
            if (implementation == "aten.cumsum:int32_to_int64")
                call.device_launches.push_back({ component_, call.work.operation + ".cast", "ascend.cast.int32_to_int64", "device_service", features });
            call.device_launches.push_back({ component_, call.work.operation + ".scan", "ascend.cumsum.int64.aicpu", "device_service", features });
        }
        else if (implementation == "aten.floor_divide:int64" && (operation == "sequence_pages" || operation == "prefix_pages")) {
            // This builder's extend prefix divides freshly produced,
            // contiguous int64 vectors by an integer scalar. On the admitted
            // 910B path both operands/output stay int64 and FloorDiv is
            // AICore. Unlike strided int32 attention page tables, no input
            // materialization or dtype conversion is required here.
            auto & call = program.back();
            call.device_launches.push_back({ component_, call.work.operation + ".divide", "ascend.floor_divide.int64.aicore", "device_service", features });
        }
        else if (component_ == "decode" && implementation == "aten.remainder:int64") {
            // On the admitted 910B tensor/scalar path, CANN broadcasts the
            // integer divisor before FloorMod. Both launches belong to one
            // worker task; no extra caller submission or dtype cast is needed.
            auto & call = program.back();
            call.device_launches = {
                { component_, call.work.operation + ".broadcast", "ascend.broadcast.int64.aicore", "device_service", features },
                { component_,    call.work.operation + ".modulo", "ascend.floor_mod.int64.aicore", "device_service", features },
            };
        }
        else if (component_ == "decode"
                 && (implementation == "aten.mul:int64_int32" || implementation == "aten.sub:int64_int32" || implementation == "aten.add:int32_int64")) {
            // These Decode operands are contiguous vectors of batch_size,
            // not scalar/broadcast inputs. The CANN integer promotion path
            // casts the int32 operand before one int64 AICore operation.
            // Add/Sub use the framework's default alpha=1, so no extra scaling.
            auto & call = program.back();
            std::string compute = "ascend.add.int64.aicore";
            if (implementation == "aten.mul:int64_int32") compute = "ascend.mul.int64.aicore";
            else if (implementation == "aten.sub:int64_int32") compute = "ascend.sub.int64.aicore";
            call.device_launches = {
                { component_,    call.work.operation + ".cast", "ascend.cast.int32_to_int64", "device_service", features },
                { component_, call.work.operation + ".compute",           std::move(compute), "device_service", features },
            };
        }
        if (device) previous_device_ = program.size() - 1;
        else previous_host_ = program.size() - 1;
        if (execution == TensorExecutionKind::BlockingContextReadback || execution == TensorExecutionKind::DirectBlockingDevice)
            previous_host_ = program.size() - 1;
        return program.size() - 1;
    }

    std::vector<TensorOperation> program;

private:
    const modules::hicache::model::HiCacheAllocatorWorkItem & call_;
    std::string component_, prefix_;
    std::optional<size_t> previous_host_, previous_device_;
};

void assign_cache_paths(const std::vector<modules::hicache::model::HiCacheAllocatorWorkItem> & calls, AllocatorPreparationWorkPlan & plan) {
    // A batch is shared across ranks, including repeated chunks of the same
    // logical request. Publish its new disk entries only after the whole batch.
    using Batch = std::pair<std::vector<std::string>, size_t>;
    std::map<std::string, std::map<std::vector<std::string>, size_t>> occurrences;
    std::map<Batch, size_t> indexes;
    std::vector<std::vector<size_t>> batches;
    for (size_t i = 0; i < calls.size(); ++i) {
        if (calls[i].phase == modules::hicache::model::HiCacheAllocatorPhase::Decode) continue;
        if (calls[i].requests.empty()) continue;

        std::vector<std::string> ids;
        for (const auto & request : calls[i].requests) ids.push_back(request.request_id);
        const Batch key{ ids, occurrences[calls[i].pid][ids]++ };
        const auto [entry, added] = indexes.emplace(key, batches.size());
        if (added) batches.emplace_back();
        batches[entry->second].push_back(i);
    }

    std::set<AllocatorSpecialization> disk;
    bool uncertain = false;
    for (const auto & batch : batches) {
        for (const auto i : batch) {
            auto & item = plan.calls[i];
            if (item.status == "already_prepared") item.path = "memory_cache";
            if (item.status == "required" && item.specialization)
                item.path = disk.contains(*item.specialization) ? "disk_cache" : uncertain ? "unknown" : "compiled";
        }

        for (const auto i : batch) {
            const auto & item = plan.calls[i];
            if (item.status == "required" && item.specialization) disk.insert(*item.specialization);
            if (item.status == "unknown" || item.status == "possibly_required" || item.status == "unsupported_signature") uncertain = true;
        }
    }
}

} // namespace

ExtendPageRegions extend_page_regions(uint64_t sequence_tokens, uint64_t prefix_tokens, uint64_t page_size_tokens) {
    const auto extend_tokens = sequence_tokens - prefix_tokens;
    // Avoid rounding the prefix upward by multiplication: a short extension
    // can stay in the existing page even near the integer range limit.
    const auto tail_capacity = (page_size_tokens - prefix_tokens % page_size_tokens) % page_size_tokens;
    const auto existing_tail = std::min(extend_tokens, tail_capacity);
    const auto remaining = extend_tokens - existing_tail;
    return { existing_tail, remaining / page_size_tokens, remaining % page_size_tokens };
}

std::optional<std::vector<TensorOperation>> ascend_decode_allocator_program(const modules::hicache::model::HiCacheAllocatorWorkItem & call,
                                                                                     const AscendAllocatorInputs & inputs) {
    // Request-to-token tables store int32 locations in the ordinary Decode
    // path; other callers may supply int64. Preserve the resulting promotion
    // and no-op-cast rules rather than borrowing the extend Triton signature.
    // Debug mode adds synchronizing assertions and is not the same program.
    if (!inputs.device_name.starts_with("Ascend910B") || call.phase != modules::hicache::model::HiCacheAllocatorPhase::Decode || !call.queue_state_known
        || inputs.debug_memory_pool || inputs.sequence_dtype != "torch.int64"
        || (inputs.last_location_dtype != "torch.int32" && inputs.last_location_dtype != "torch.int64") || inputs.free_page_dtype != "torch.int64")
        return std::nullopt;

    const bool last_int32 = inputs.last_location_dtype == "torch.int32";
    AllocatorProgram builder(call, "decode");
    // get_num_new_pages reads the CPU sequence lengths. It is CPU service,
    // not a D2H transfer or a measured wait from the base allocation.
    if (call.merge_release_queue) {
        const auto pages = core::checked_add_u64(call.free_pages_before, call.release_pages_before, "Allocator merge page count overflow");
        const auto joined = builder.append("merge", "aten.cat:int64", pages, {});
        builder.append("sort", "aten.sort:int64", pages, { joined });
    }

    const auto remainder = builder.append("remainder", "aten.remainder:int64", call.batch_size, {});
    const auto needs = builder.append("needs_page", "aten.eq:int64", call.batch_size, { remainder });
    const auto needs_int = builder.append("needs_page_int", "aten._to_copy:bool_to_int32", call.batch_size, { needs });
    const auto end = builder.append("end_pages", "aten.cumsum:int32_to_int64", call.batch_size, { needs_int });
    // The framework computes this even when new_pages == 0. Do not prune it
    // just because that branch will not consume its result.
    const auto start = builder.append("start_pages", "aten.sub:int64_int32", call.batch_size, { end, needs_int });
    const auto next = builder.append("next_location", last_int32 ? "aten.add:int32" : "aten.add:int64", call.batch_size, {});
    auto output = next;
    if (call.allocated_pages > 0) {
        const auto keep = builder.append("keep_page", "aten.rsub:int32", call.batch_size, { needs_int });
        const auto existing = builder.append("existing_location", last_int32 ? "aten.mul:int32" : "aten.mul:int64_int32", call.batch_size, { next, keep });
        const auto selected = builder.append("new_pages", "aten.index:int64", call.batch_size, { start });
        const auto offsets = builder.append("page_offsets", "aten.mul:int64", call.batch_size, { selected });
        const auto new_location = builder.append("new_location", "aten.mul:int64_int32", call.batch_size, { offsets, needs_int });
        output = builder.append("output_location", last_int32 ? "aten.add:int32_int64" : "aten.add:int64", call.batch_size, { existing, new_location });
    }
    // No new pages + int32 last_loc already returns int32; .int() is then a
    // host-only no-op, not an extra device cast or a missing cost sample.
    if (call.allocated_pages > 0 || !last_int32) builder.append("output_int", "aten._to_copy:int64_to_int32", call.batch_size, { output });
    builder.append("return", "sglang.NPUPagedTokenToKVPoolAllocator", call.batch_size, {}, TensorExecutionKind::CpuService);
    return std::move(builder.program);
}

std::optional<std::vector<TensorOperation>> ascend_extend_allocator_prefix(const modules::hicache::model::HiCacheAllocatorWorkItem & call,
                                                                                    const AscendAllocatorInputs & inputs, const std::string & prefix_dtype,
                                                                                    bool page_count_supplied) {
    if (!inputs.device_name.starts_with("Ascend910B") || call.phase != modules::hicache::model::HiCacheAllocatorPhase::Extend || !call.queue_state_known
        || inputs.debug_memory_pool || prefix_dtype != "torch.int64" || inputs.sequence_dtype != "torch.int64" || inputs.free_page_dtype != "torch.int64")
        return std::nullopt;

    AllocatorProgram builder(call, "prefill");
    if (!page_count_supplied) {
        const auto seq_round = builder.append("sequence_roundup", "aten.add:int64", call.batch_size, {});
        const auto seq_pages = builder.append("sequence_pages", "aten.floor_divide:int64", call.batch_size, { seq_round });
        const auto pre_round = builder.append("prefix_roundup", "aten.add:int64", call.batch_size, {});
        const auto pre_pages = builder.append("prefix_pages", "aten.floor_divide:int64", call.batch_size, { pre_round });
        const auto counts = builder.append("new_pages", "aten.sub:int64", call.batch_size, { seq_pages, pre_pages });
        const auto total = builder.append("sum_pages", "aten.sum:int64", call.batch_size, { counts });
        builder.append("page_count_readback", "aten._local_scalar_dense:int64", 1, { total }, TensorExecutionKind::BlockingContextReadback);
    }
    if (call.merge_release_queue) {
        const auto pages = core::checked_add_u64(call.free_pages_before, call.release_pages_before, "Allocator merge page count overflow");
        const auto joined = builder.append("merge", "aten.cat:int64", pages, {});
        builder.append("sort", "aten.sort:int64", pages, { joined });
    }
    return std::move(builder.program);
}

OperationWork allocator_preparation_operation(const AllocatorPreparation & preparation) {
    const auto & shape = preparation.specialization.value();
    return {
        .component = "execution_control", .operation = "allocator.preparation", .implementation = preparation.path,
        .timing_scope = "cpu_service",
        .features = {
            {"page_size_tokens", static_cast<double>(shape[0])}, {"batch_upper_bound", static_cast<double>(shape[1])},
            {"extend_upper_bound_tokens", static_cast<double>(shape[2])}, {"free_pointer_mod16_bytes", static_cast<double>(shape[3])},
            {"first_load", preparation.first_load ? 1.0 : 0.0},
        },
    };
}

std::optional<std::vector<TensorOperation>> ascend_fused_extend_allocator_program(const modules::hicache::model::HiCacheAllocatorWorkItem & call,
                                                                                           const AscendAllocatorInputs & inputs,
                                                                                           const std::string & prefix_dtype, bool page_count_supplied,
                                                                                           const AllocatorPreparation & preparation) {
    if (call.allocated_pages >= kNpuNaiveAllocationMinimumPages || inputs.last_location_dtype != "torch.int64" || !inputs.triton_task_queue_enabled.has_value()
        || !preparation.specialization || (preparation.status != "required" && preparation.status != "already_prepared"))
        return std::nullopt;
    auto prefix = ascend_extend_allocator_prefix(call, inputs, prefix_dtype, page_count_supplied);
    if (!prefix) return std::nullopt;

    AllocatorProgram builder(call, "prefill");
    builder.adopt_prefix(std::move(*prefix));
    if (preparation.status == "required") {
        const auto slot = builder.append("preparation", preparation.path, 0, {}, TensorExecutionKind::CpuService);
        builder.program[slot].work = allocator_preparation_operation(preparation);
    }
    const auto execution = *inputs.triton_task_queue_enabled ? TensorExecutionKind::AsyncDevice : TensorExecutionKind::DirectBlockingDevice;
    const auto kernel = builder.append("fused_indices", "sgl_kernel_npu.alloc_extend_kernel:int64", call.extend_tokens, {}, execution);
    const auto & shape = *preparation.specialization;
    auto & features = builder.program[kernel].work.features;
    features.emplace("triton_task_queue_enabled", *inputs.triton_task_queue_enabled ? 1.0 : 0.0);
    features.emplace("batch_upper_bound", static_cast<double>(shape[1]));
    features.emplace("extend_upper_bound_tokens", static_cast<double>(shape[2]));
    features.emplace("free_pointer_mod16_bytes", static_cast<double>(shape[3]));
    builder.append("output_int", "aten._to_copy:int64_to_int32", call.extend_tokens, { kernel });
    builder.append("return", "sglang.NPUPagedTokenToKVPoolAllocator", call.batch_size, {}, TensorExecutionKind::CpuService);
    return std::move(builder.program);
}

std::optional<std::vector<TensorOperation>> ascend_naive_extend_allocator_program(const modules::hicache::model::HiCacheAllocatorWorkItem & call,
                                                                                           const AscendAllocatorInputs & inputs,
                                                                                           const std::string & prefix_dtype, bool page_count_supplied) {
    if (call.allocated_pages < kNpuNaiveAllocationMinimumPages || inputs.last_location_dtype != "torch.int64" || call.requests.size() != call.batch_size)
        return std::nullopt;
    auto prefix = ascend_extend_allocator_prefix(call, inputs, prefix_dtype, page_count_supplied);
    if (!prefix) return std::nullopt;

    AllocatorProgram builder(call, "prefill");
    builder.adopt_prefix(std::move(*prefix));
    const auto op = [&](const std::string & name, const std::string & implementation, uint64_t elements, std::vector<size_t> dependencies) {
        return builder.append("naive." + name, implementation, elements, std::move(dependencies));
    };
    const auto scalar = [&](const std::string & name, const std::string & implementation, std::vector<size_t> dependencies) {
        return op(name, implementation, 1, std::move(dependencies));
    };
    const auto read = [&](const std::string & name, size_t value, bool boolean = false) {
        return builder.append("naive." + name,
                              boolean ? "aten._local_scalar_dense:bool" : "aten._local_scalar_dense:int64",
                              1,
                              { value },
                              TensorExecutionKind::BlockingContextReadback);
    };
    const auto rounded_pages = [&](const std::string & name, uint64_t elements) {
        // The Python expression is x + page_size - 1, not x + roundup.
        // Its two tensor calls must not be silently optimized into one.
        const auto plus = op(name + ".plus_page", "aten.add:int64", elements, {});
        const auto minus = op(name + ".minus_one", "aten.sub:int64", elements, { plus });
        return op(name + ".pages", "aten.floor_divide:int64", elements, { minus });
    };

    const auto lengths = op("extend_lengths", "aten.sub:int64", call.batch_size, {});
    const auto end = op("end_positions", "aten.cumsum:int64", call.batch_size, { lengths });
    const auto start = op("start_positions", "aten.sub:int64", call.batch_size, { end, lengths });
    const auto sequence_pages = rounded_pages("sequence", call.batch_size);
    const auto prefix_pages = rounded_pages("prefix", call.batch_size);
    const auto new_pages = op("new_pages", "aten.sub:int64", call.batch_size, { sequence_pages, prefix_pages });
    const auto floor_pages = op("sequence_full_pages", "aten.floor_divide:int64", call.batch_size, {});
    const auto full_prefix = rounded_pages("full_prefix", call.batch_size);
    const auto full_pages = op("full_pages", "aten.sub:int64", call.batch_size, { floor_pages, full_prefix });
    const auto need_tail = op("need_tail", "aten.sub:int64", call.batch_size, { new_pages, full_pages });
    const auto page_end = op("end_pages", "aten.cumsum:int64", call.batch_size, { new_pages });
    const auto page_start = op("start_pages", "aten.sub:int64", call.batch_size, { page_end, new_pages });
    const auto positions = op("page_positions", "aten.arange:int32", call.page_size, {});

    const auto mark_request = [&](size_t first, size_t request) {
        for (size_t i = first; i < builder.program.size(); ++i) builder.program[i].request_index = request;
    };
    for (size_t i = 0; i < call.requests.size(); ++i) {
        const auto first = builder.program.size();
        const auto & request = call.requests[i];
        const auto regions = extend_page_regions(request.sequence_tokens, request.prefix_tokens, call.page_size);
        const auto tokens = request.sequence_tokens - request.prefix_tokens;
        const auto rounded = rounded_pages("request_prefix", 1);
        const auto rounded_tokens = scalar("rounded_prefix_tokens", "aten.mul:int64", { rounded });
        const auto smaller = scalar("prefix_before_sequence", "aten.lt:int64", { rounded_tokens });
        read("minimum_branch", smaller, true);
        // Python min keeps its first (sequence) operand on equality. The
        // rounded prefix is selected only when the extension crosses its end.
        const auto selected_minimum = regions.existing_tail_tokens < tokens ? std::vector<size_t>{ rounded_tokens } : std::vector<size_t>{};
        const auto first_count = scalar("existing_tail_count", "aten.sub:int64", selected_minimum);
        read("has_existing_tail", first_count);
        if (regions.existing_tail_tokens) {
            // Assignment evaluates RHS before constructing/resolving the LHS
            // slice. A slice's tensor bounds are both computed before either
            // bound is converted to a Python index.
            const auto next = scalar("next_location", "aten.add:int64", {});
            read("existing_position_stop", first_count);
            const auto values = op("existing_values", "aten.add:scalar_int64_vector_int32", regions.existing_tail_tokens, { next, positions });
            const auto stop = scalar("existing_output_stop", "aten.add:int64", { start, first_count });
            read("existing_output_begin", start);
            read("existing_output_end", stop);
            op("existing_copy", "aten.copy_:to_int32", regions.existing_tail_tokens, { values });
        }
        const auto after_first = scalar("after_existing_tail", "aten.add:int64", { first_count });
        const auto first_done = scalar("existing_finishes", "aten.eq:int64", { after_first });
        read("existing_finished_branch", first_done, true);
        if (regions.existing_tail_tokens == tokens) {
            mark_request(first, i);
            continue;
        }

        const auto seq_full = scalar("request_full_sequence", "aten.floor_divide:int64", {});
        const auto pre_full = rounded_pages("request_full_prefix", 1);
        const auto full_count = scalar("request_full_pages", "aten.sub:int64", { seq_full, pre_full });
        const auto second_count = scalar("full_token_count", "aten.mul:int64", { full_count });
        read("has_full_pages", second_count);
        const auto full_tokens = core::checked_multiply_u64(regions.new_full_pages, call.page_size, "Allocator full-region size overflow");
        if (full_tokens) {
            const auto stop_page = scalar("full_page_stop", "aten.sub:int64", { page_end, need_tail });
            read("full_page_begin", page_start);
            read("full_page_end", stop_page);
            const auto offsets = op("full_offsets", "aten.mul:int64", regions.new_full_pages, { page_start, stop_page });
            const auto values = op("full_values", "aten.add:int64_int32", full_tokens, { offsets, positions });
            const auto begin_output = scalar("full_output_begin_value", "aten.add:int64", { start, first_count });
            const auto end_prefix = scalar("full_output_end_prefix", "aten.add:int64", { start, first_count });
            const auto end_output = scalar("full_output_end_value", "aten.add:int64", { end_prefix, second_count });
            read("full_output_begin", begin_output);
            read("full_output_end", end_output);
            op("full_copy", "aten.copy_:to_int32", full_tokens, { values });
        }
        const auto done_prefix = scalar("after_full_prefix", "aten.add:int64", { first_count });
        const auto after_full = scalar("after_full_pages", "aten.add:int64", { done_prefix, second_count });
        const auto full_done = scalar("full_finishes", "aten.eq:int64", { after_full });
        read("full_finished_branch", full_done, true);
        if (!regions.new_tail_tokens) {
            mark_request(first, i);
            continue;
        }

        const auto tail_floor = scalar("tail_floor_pages", "aten.floor_divide:int64", {});
        const auto tail_floor_tokens = scalar("tail_floor_tokens", "aten.mul:int64", { tail_floor });
        const auto third_count = scalar("tail_token_count", "aten.sub:int64", { tail_floor_tokens });
        read("has_new_tail", third_count);
        const auto last_page = scalar("last_page_index", "aten.sub:int64", { page_end });
        read("last_page_lookup", last_page);
        const auto offset = scalar("last_page_offset", "aten.mul:int64", { last_page });
        read("tail_position_stop", third_count);
        const auto values = op("tail_values", "aten.add:scalar_int64_vector_int32", regions.new_tail_tokens, { offset, positions });
        const auto begin_output = scalar("tail_output_begin_value", "aten.sub:int64", { end, third_count });
        read("tail_output_begin", begin_output);
        read("tail_output_end", end);
        op("tail_copy", "aten.copy_:to_int32", regions.new_tail_tokens, { values });
        mark_request(first, i);
    }
    // Naive output was allocated int32, so the enclosing .int() emits no cast.
    builder.append("return", "sglang.NPUPagedTokenToKVPoolAllocator", call.batch_size, {}, TensorExecutionKind::CpuService);
    return std::move(builder.program);
}

bool allocator_work_unchanged(const modules::hicache::model::HiCacheAllocatorWorkItem & source,
                              const modules::hicache::model::HiCacheAllocatorWorkItem & target) {
    if (!source.queue_state_known || !target.queue_state_known || !source.free_index_offset || !target.free_index_offset || source.phase != target.phase
        || source.page_size != target.page_size || source.batch_size != target.batch_size || source.extend_tokens != target.extend_tokens
        || source.allocated_pages != target.allocated_pages || source.free_pages_before != target.free_pages_before
        || source.release_pages_before != target.release_pages_before || source.merge_release_queue != target.merge_release_queue
        || source.free_index_offset != target.free_index_offset || source.requests.size() != target.requests.size())
        return false;

    return std::ranges::equal(source.requests, target.requests, [](const auto & left, const auto & right) {
        return left.request_id == right.request_id && left.sequence_tokens == right.sequence_tokens && left.prefix_tokens == right.prefix_tokens
               && left.decode_iteration == right.decode_iteration;
    });
}

AllocatorPreparationWorkPlan plan_allocator_work(const std::set<std::string> & supported_pids,
                                                 const std::vector<modules::hicache::model::HiCacheAllocatorWorkItem> & calls) {
    AllocatorPreparationWorkPlan result;
    std::map<std::string, std::set<AllocatorSpecialization>> prepared;
    std::set<std::string> uncertain;
    for (const auto & call : calls) {
        AllocatorPreparation item;
        // Ascend Decode uses the eager tensor program, not the extend Triton
        // specialization cache. Its calls must not advance first-use history.
        if (call.phase == modules::hicache::model::HiCacheAllocatorPhase::Decode) {
            item.status = "not_required";
            item.path = "eager_decode";
            result.calls.push_back(std::move(item));
            continue;
        }
        if (!supported_pids.contains(call.pid)) item.status = "unsupported_signature";
        else if (call.allocated_pages >= kNpuNaiveAllocationMinimumPages) item.status = "naive_path";
        else if (!call.free_index_offset || !call.extend_tokens || !call.batch_size || call.extend_tokens > (uint64_t{ 1 } << 63)
                 || call.batch_size > (uint64_t{ 1 } << 63)) {
            item.status = "unknown";
            uncertain.insert(call.pid);
        }
        else {
            item.specialization =
                AllocatorSpecialization{ call.page_size, std::bit_ceil(call.batch_size), std::bit_ceil(call.extend_tokens), (*call.free_index_offset % 2) * 8 };
            auto & cache = prepared[call.pid];
            item.first_load = cache.empty();
            item.status = cache.contains(*item.specialization) ? "already_prepared" : uncertain.contains(call.pid) ? "possibly_required" : "required";
            cache.insert(*item.specialization);
        }

        if (call.formal && (item.status == "unknown" || item.status == "possibly_required" || item.status == "unsupported_signature"))
            ++result.blockers[item.status];
        result.calls.push_back(std::move(item));
    }

    assign_cache_paths(calls, result);
    result.status = !result.blockers.empty() || calls.empty() ? "partial" : "ready";
    return result;
}

} // namespace markov::trace_graph::structure::hicache
