#include "markov/adapters/frameworks/sglang/bindings.hpp"
#include "markov/adapters/frameworks/sglang/models/qwen3_regions.hpp"

#include "markov/graph/source_graph.hpp"
#include "markov/production/source_call.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

namespace markov {
namespace {

nlohmann::json describe_parameters(std::span<const NativeRegionParameter> parameters) {
    auto result = nlohmann::json::array();
    for (const auto & parameter : parameters)
        result.push_back({
            {      "name",      parameter.name },
            { "attribute", parameter.attribute }
        });
    return result;
}

} // namespace

std::expected<ServingLimits, std::string_view> SGLangBindings::control_limits(const SGLangConfiguration & configuration) const {
    // Native flags must qualify the common policy before capacities can be used.
    if (configuration.device != "npu" || configuration.attention_backend != "ascend" || configuration.schedule_policy != "fcfs"
        || !configuration.disable_radix_cache || !configuration.disable_overlap_schedule || !configuration.disable_cuda_graph
        || configuration.chunked_prefill_size != -1 || configuration.enable_mixed_chunk || configuration.enable_hierarchical_cache)
        return std::unexpected("control_path_not_implemented");

    return ServingLimits{ configuration.max_running_requests, configuration.max_total_tokens, configuration.max_prefill_tokens };
}

std::uint32_t SGLangBindings::request_capacity(const nlohmann::json & environment) const {
    const auto & arguments = environment.at("effective_server_args");
    const auto & capacity = arguments.at("pp_max_micro_batch_size");
    if (!capacity.is_null() && capacity != 0) return capacity.get<std::uint32_t>();

    // Scheduler.init_model_worker fills this default after worker setup.
    return std::max(arguments.at("max_running_requests").get<std::uint32_t>() / arguments.at("pp_size").get<std::uint32_t>(), 1U);
}

std::optional<AdmissionStop> SGLangBindings::admission_stop(const SourceNativeRegion & region, std::uint32_t request_capacity) const {
    const auto & work = region.result.at("admission_work");
    const auto & stop = work.at("native_stop");
    if (stop == "queue_exhausted") return AdmissionStop::QueueExhausted;
    if (stop == "request_capacity") return AdmissionStop::RequestCapacity;
    if (stop != "early_return" || work.at("candidate_checks") != 0) return std::nullopt;

    const auto & entry = region.call_parameters;
    if (entry.at("batch_is_full") != 0) return AdmissionStop::Closed;
    if (entry.at("waiting_requests") == 0) return AdmissionStop::NoWaitingRequests;
    // Native admission can set batch_is_full and return before visiting a
    // candidate. Its entry snapshot still has the previous latch value.
    if (entry.at("running_requests") >= static_cast<std::int64_t>(request_capacity)) return AdmissionStop::RequestCapacity;
    return std::nullopt;
}

SGLangBindings::WorkerControlQualification SGLangBindings::qualify_worker_controls(const nlohmann::json & metadata) const {
    const auto controls = metadata.find("worker_controls");
    if (controls == metadata.end() || controls->is_null()) return WorkerControlQualification::NotObserved;

    // Missing native policy facts are unknown; launcher defaults cannot qualify them.
    static constexpr std::array fields{ "waiting_timeout_seconds", "running_timeout_seconds", "test_retract",
                                        "require_mlp_sync",        "max_recv_per_poll",       "recv_skipper",
                                        "input_blocker",           "scripted_scheduler_hook", "idle_sleeper" };
    for (const auto field : fields)
        if (!controls->contains(field) || controls->at(field).is_null()) return WorkerControlQualification::NotObserved;

    const auto waiting_timeout = controls->at("waiting_timeout_seconds").get<double>();
    const auto running_timeout = controls->at("running_timeout_seconds").get<double>();
    if (!std::isfinite(waiting_timeout) || !std::isfinite(running_timeout)) throw std::invalid_argument("native request timeouts must be finite seconds");

    if (waiting_timeout > 0 || running_timeout > 0 || controls->at("test_retract").get<bool>() || controls->at("require_mlp_sync").get<bool>()
        || controls->at("max_recv_per_poll").get<std::int64_t>() >= 0 || controls->at("recv_skipper").get<bool>() || controls->at("input_blocker").get<bool>()
        || controls->at("scripted_scheduler_hook").get<bool>() || controls->at("idle_sleeper").get<bool>())
        return WorkerControlQualification::Unsupported;
    return WorkerControlQualification::Supported;
}

bool SGLangBindings::supports_configuration_change(const SGLangConfiguration & source, SGLangConfiguration target) const {
    // Admission limits select batches; each resulting template still needs its
    // own shape, control-path and resource qualification.
    target.max_running_requests = source.max_running_requests;
    target.max_prefill_tokens = source.max_prefill_tokens;
    return target == source;
}

bool SGLangBindings::supports_native_path(const nlohmann::json & environment, std::uint32_t physical_tp_size) const {
    const auto & actual = environment.at("effective_server_args");
    for (const auto & [name, value] : environment.at("configuration").items()) {
        const auto expected = name == "tp_size" ? nlohmann::json(physical_tp_size) : value;
        if (actual.at(name) != expected) return false;
    }

    // Interpret the observed native path at the production boundary. Reuse the
    // same serving policy as planning rather than copying it into extraction.
    const auto configuration = SGLangConfiguration::from_parameters(environment.at("configuration"));
    return control_limits(configuration).has_value() && Qwen3Regions::supports(environment.at("model_configuration"))
           && actual.at("speculative_algorithm").is_null() && actual.at("rl_on_policy_target").is_null() && !actual.at("enable_hisparse").get<bool>()
           && actual.at("dllm_algorithm").is_null() && !actual.at("enable_priority_scheduling").get<bool>() && !actual.at("enable_prefill_delayer").get<bool>()
           && !actual.at("enable_deterministic_inference").get<bool>() && !actual.at("enable_custom_logit_processor").get<bool>();
}

std::span<const BlockBinding> SGLangBindings::bindings() const {
    static constexpr std::array selected{
        BlockPortBinding{ PortKind::Selected, NativePortAnchor::ScheduleDecision }
    };
    static constexpr std::array prepared{
        BlockPortBinding{ PortKind::Prepared, NativePortAnchor::ParentScheduleReturn }
    };
    static constexpr std::array executed{
        BlockPortBinding{  PortKind::Submitted,           NativePortAnchor::ScopeReturn },
        BlockPortBinding{ PortKind::DeviceDone, NativePortAnchor::OwnedDeviceCompletion }
    };
    static constexpr std::array processed{
        BlockPortBinding{ PortKind::ResultProcessed, NativePortAnchor::ScopeReturn }
    };

    static constexpr BlockBinding schedule{ "sglang.schedule",
                                            StepKind::Schedule,
                                            std::nullopt,
                                            "sglang.srt.managers.scheduler_components.request_receiver.SchedulerRequestReceiver.recv_requests",
                                            NativeWrapperKind::Schedule,
                                            selected };
    static constexpr BlockBinding prepare_prefill{ "sglang.prepare.prefill",   StepKind::Prepare,
                                                   BatchPhase::Prefill,        "sglang.srt.managers.schedule_batch.ScheduleBatch.prepare_for_extend",
                                                   NativeWrapperKind::Prepare, prepared };
    static constexpr BlockBinding prepare_decode{ "sglang.prepare.decode",    StepKind::Prepare,
                                                  BatchPhase::Decode,         "sglang.srt.managers.schedule_batch.ScheduleBatch.prepare_for_decode",
                                                  NativeWrapperKind::Prepare, prepared };
    static constexpr BlockBinding prefill{ "sglang.prefill",           StepKind::Prefill,
                                           BatchPhase::Prefill,        "sglang.srt.managers.scheduler.Scheduler.run_batch",
                                           NativeWrapperKind::Execute, executed };
    static constexpr BlockBinding decode{
        "sglang.decode", StepKind::Decode, BatchPhase::Decode, "sglang.srt.managers.scheduler.Scheduler.run_batch", NativeWrapperKind::Execute, executed
    };
    static constexpr BlockBinding publish{
        "sglang.publish", StepKind::Publish, std::nullopt, "sglang.srt.managers.scheduler.Scheduler.process_batch_result", NativeWrapperKind::Publish, processed
    };
    static constexpr std::array values{ schedule, prepare_prefill, prepare_decode, prefill, decode, publish };
    return values;
}

const BlockBinding & SGLangBindings::binding(std::string_view identity) const {
    for (const auto & value : bindings()) {
        if (value.identity == identity) return value;
    }

    throw std::invalid_argument("unknown SGLang block binding: " + std::string(identity));
}

nlohmann::json SGLangBindings::describe() const {
    const ServingContract contract;
    const std::array wrappers{ "schedule", "prepare", "execute", "publish" };
    auto result = nlohmann::json::array();
    for (const auto & value : bindings()) {
        nlohmann::json phase = nullptr;
        if (value.phase) phase = *value.phase == BatchPhase::Prefill ? "prefill" : "decode";
        result.push_back({
            {      "id",                                    value.identity },
            {    "step",                   contract.block(value.step).name },
            {   "phase",                                             phase },
            {    "hook",                                        value.hook },
            { "wrapper", wrappers[static_cast<std::size_t>(value.wrapper)] }
        });
    }

    return result;
}

std::span<const NativeRegionBinding> SGLangBindings::serving_regions() const {
    static constexpr std::array<std::string_view, 1> collective_inputs{ "input_" };
    static constexpr std::array collective_parameters{
        NativeRegionParameter{  "logical_tp_size", "logical_tp_size" },
        NativeRegionParameter{     "logical_rank",    "logical_rank" },
        NativeRegionParameter{ "physical_tp_size",      "world_size" }
    };
    static constexpr std::array collective_outputs{
        NativeRegionOutput{ "output", std::nullopt }
    };
    static constexpr std::array gather_parameters{
        NativeRegionParameter{ "dim", "dim" }
    };
    static constexpr std::array<std::string_view, 2> extend_cpu_inputs{ "prefix_lens_cpu", "seq_lens_cpu" };
    static constexpr std::array<std::string_view, 1> decode_cpu_inputs{ "seq_lens_cpu" };
    static constexpr std::array allocation_parameters{
        NativeRegionParameter{            "page_size",            "page_size" },
        NativeRegionParameter{            "need_sort",            "need_sort" },
        NativeRegionParameter{ "is_not_in_free_group", "is_not_in_free_group" }
    };
    static constexpr std::array extend_parameters{
        NativeRegionParameter{ "extend_num_tokens", "extend_num_tokens" }
    };
    static constexpr std::array allocation_inputs{
        NativeRegionParameter{ "free_pages", "free_pages" }
    };
    static constexpr std::array<std::string_view, 1> release_inputs{ "free_index" };
    static constexpr std::array allocation_outputs{
        NativeRegionOutput{ "out_cache_loc", std::nullopt }
    };
    using namespace std::string_view_literals;
    static constexpr std::array metadata_inputs{ "forward_batch.seq_lens"sv };
    static constexpr std::array metadata_cpu_inputs{ "forward_batch.seq_lens_cpu"sv };
    static constexpr std::array metadata_parameters{
        NativeRegionParameter{     "page_size",     "page_size" },
        NativeRegionParameter{ "is_hybrid_swa", "is_hybrid_swa" }
    };
    static constexpr std::array metadata_call_parameters{
        NativeRegionParameter{ "forward_mode", "forward_batch.forward_mode" }
    };
    static constexpr std::array sampling_parameters{
        NativeRegionParameter{   "forward_mode",                "forward_batch.forward_mode" },
        NativeRegionParameter{ "return_logprob",              "forward_batch.return_logprob" },
        NativeRegionParameter{  "is_all_greedy", "forward_batch.sampling_info.is_all_greedy" }
    };
    static constexpr std::array prefix_publication_parameters{
        NativeRegionParameter{   "sequence_tokens",     "req.fill_len" },
        NativeRegionParameter{ "request_pool_slot", "req.req_pool_idx" }
    };
    static constexpr std::array merge_parameters{
        NativeRegionParameter{ "filter_prefix_count", "filter_prefix_count" },
        NativeRegionParameter{       "left_requests",       "left_requests" },
        NativeRegionParameter{      "right_requests",      "right_requests" }
    };
    static constexpr std::array filter_parameters{
        NativeRegionParameter{ "filter_index", "filter_index" }
    };
    static constexpr std::array request_publication_parameters{
        NativeRegionParameter{ "request_index", "i" }
    };
    static constexpr std::array output_parameters{
        NativeRegionParameter{ "is_generation", "is_generation" }
    };
    static constexpr std::array output_call_parameters{
        NativeRegionParameter{ "return_logprob", "return_logprob" }
    };
    static constexpr std::array last_location_member_parameters{
        NativeRegionParameter{ "request_index", "request_index" },
        NativeRegionParameter{   "cold_prefix",   "cold_prefix" }
    };
    static constexpr std::array last_location_parameters{
        NativeRegionParameter{ "request_count", "request_count" },
        NativeRegionParameter{   "cold_prefix",   "cold_prefix" }
    };
    static constexpr std::array admission_parameters{
        NativeRegionParameter{ "waiting_requests", "waiting_requests" },
        NativeRegionParameter{ "running_requests", "running_requests" },
        NativeRegionParameter{    "batch_is_full",    "batch_is_full" }
    };
    static constexpr std::array statistics_parameters{
        NativeRegionParameter{    "waiting_requests",    "waiting_requests" },
        NativeRegionParameter{    "running_requests",    "running_requests" },
        NativeRegionParameter{ "has_chunked_request", "has_chunked_request" }
    };
    static constexpr std::array admission_budget_parameters{
        NativeRegionParameter{  "running_requests",  "running_requests" },
        NativeRegionParameter{ "accepted_requests", "accepted_requests" },
        NativeRegionParameter{      "input_tokens",      "input_tokens" }
    };
    static constexpr std::array transition_parameters{
        NativeRegionParameter{         "running_requests",         "running_requests" },
        NativeRegionParameter{      "has_pending_prefill",      "has_pending_prefill" },
        NativeRegionParameter{ "pending_prefill_requests", "pending_prefill_requests" },
        NativeRegionParameter{            "batch_is_full",            "batch_is_full" }
    };
    static constexpr std::array decode_update_parameters{
        NativeRegionParameter{       "request_count",       "request_count" },
        NativeRegionParameter{       "batch_is_full",       "batch_is_full" },
        NativeRegionParameter{ "filter_prefix_count", "filter_prefix_count" }
    };
    static constexpr std::array input_resolution_inputs{
        NativeRegionParameter{ "prefill_input_ids_cpu", "prefill_input_ids_cpu" }
    };
    static constexpr std::array input_receive_parameters{
        NativeRegionParameter{ "logical_tp_size", "ps.tp_size" },
        NativeRegionParameter{ "logical_tp_rank", "ps.tp_rank" }
    };
    static constexpr std::array values{
        NativeRegionBinding{ .identity = "sglang.schedule.receive",
                            .semantic = "input_receive",
                            .module_type = "sglang.srt.managers.scheduler_components.request_receiver.SchedulerRequestReceiver",
                            .method = "recv_requests",
                            .parameters = input_receive_parameters,
                            .hook = "sglang.srt.managers.scheduler_components.request_receiver.SchedulerRequestReceiver.recv_requests" },
        NativeRegionBinding{ .identity = "sglang.schedule.input_pull",
                            .semantic = "input_pull",
                            .module_type = "sglang.srt.managers.scheduler_components.request_receiver.SchedulerRequestReceiver",
                            .method = "_pull_raw_reqs",
                            .hook = "sglang.srt.managers.scheduler_components.request_receiver.SchedulerRequestReceiver._pull_raw_reqs" },
        NativeRegionBinding{ .identity = "sglang.schedule.input_broadcast",
                            .semantic = "input_broadcast",
                            .module_type = "sglang.srt.managers.scheduler_components.request_receiver.SchedulerRequestReceiver",
                            .method = "_broadcast_reqs_across_ranks",
                            .parameters = input_receive_parameters,
                            .hook = "sglang.srt.managers.scheduler_components.request_receiver.SchedulerRequestReceiver._broadcast_reqs_across_ranks" },
        NativeRegionBinding{ .identity = "sglang.schedule.input_message",
                            .semantic = "input_message",
                            .module_type = "torch.distributed",
                            .method = "broadcast",
                            .hook = "torch.distributed.broadcast" },
        NativeRegionBinding{ .identity = "sglang.schedule.input_message_submit",
                            .semantic = "input_message_submit",
                            .module_type = "torch.distributed.ProcessGroup",
                            .method = "broadcast",
                            .hook = "torch.distributed.ProcessGroup.broadcast" },
        NativeRegionBinding{ .identity = "sglang.schedule.input_message_wait",
                            .semantic = "input_message_wait",
                            .module_type = "torch.distributed.Work",
                            .method = "wait",
                            .hook = "torch.distributed.Work.wait" },
        NativeRegionBinding{ .identity = "sglang.schedule.input_process",
                            .semantic = "input_process",
                            .module_type = "sglang.srt.managers.scheduler.Scheduler",
                            .method = "process_input_requests",
                            .hook = "sglang.srt.managers.scheduler.Scheduler.process_input_requests" },
        NativeRegionBinding{ .identity = "sglang.schedule.input_request",
                            .semantic = "input_request",
                            .module_type = "sglang.srt.managers.scheduler.Scheduler",
                            .method = "handle_generate_request",
                            .request_id_path = "recv_req.rid",
                            .hook = "sglang.srt.managers.scheduler.Scheduler.handle_generate_request" },
        NativeRegionBinding{ .identity = "sglang.schedule.idle",
                            .semantic = "idle_suffix",
                            .module_type = "sglang.srt.managers.scheduler.Scheduler",
                            .method = "on_idle",
                            .hook = "sglang.srt.managers.scheduler.Scheduler.on_idle" },
        NativeRegionBinding{ .identity = "sglang.inputs.resolve",
                            .semantic = "input_resolution",
                            .module_type = "sglang.srt.managers.overlap_utils",
                            .method = "resolve_forward_inputs",
                            .module_inputs = input_resolution_inputs,
                            .component_path = ".",
                            .hook = "sglang.srt.managers.overlap_utils.resolve_forward_inputs" },
        NativeRegionBinding{ .identity = "sglang.schedule.decode_update",
                            .semantic = "decode_update_prefix",
                            .module_type = "sglang.srt.managers.scheduler.Scheduler",
                            .method = "update_running_batch",
                            .call_parameters = decode_update_parameters,
                            .hook = "sglang.srt.managers.scheduler.Scheduler.update_running_batch" },
        NativeRegionBinding{ .identity = "sglang.schedule.transition",
                            .semantic = "batch_transition",
                            .module_type = "sglang.srt.managers.scheduler.Scheduler",
                            .method = "get_next_batch_to_run",
                            .call_parameters = transition_parameters,
                            .hook = "sglang.srt.managers.scheduler.Scheduler.get_next_batch_to_run" },
        NativeRegionBinding{ .identity = "sglang.schedule.admission",
                            .semantic = "admission_prefix",
                            .module_type = "sglang.srt.managers.scheduler.Scheduler",
                            .method = "_get_new_batch_prefill_raw",
                            .call_parameters = admission_parameters,
                            .hook = "sglang.srt.managers.scheduler.Scheduler._get_new_batch_prefill_raw" },
        NativeRegionBinding{ .identity = "sglang.schedule.admission_budget",
                            .semantic = "admission_budget",
                            .module_type = "sglang.srt.managers.schedule_policy.PrefillAdder",
                            .method = "add_one_req_ignore_eos",
                            .call_parameters = admission_budget_parameters,
                            .hook = "sglang.srt.managers.schedule_policy.PrefillAdder.add_one_req_ignore_eos" },
        NativeRegionBinding{ .identity = "sglang.schedule.prefill_statistics",
                            .semantic = "prefill_statistics",
                            .module_type = "sglang.srt.managers.scheduler.Scheduler",
                            .method = "_get_new_batch_prefill_raw",
                            .call_parameters = statistics_parameters,
                            .hook = "sglang.srt.managers.scheduler.Scheduler._get_new_batch_prefill_raw" },
        NativeRegionBinding{ .identity = "sglang.prepare.last_locations",
                            .semantic = "last_locations",
                            .module_type = "sglang.srt.mem_cache.common",
                            .method = "alloc_for_extend",
                            .call_parameters = last_location_parameters,
                            .hook = "sglang.srt.managers.schedule_batch.alloc_for_extend" },
        NativeRegionBinding{ .identity = "sglang.prepare.last_location",
                            .semantic = "last_location",
                            .module_type = "sglang.srt.mem_cache.common",
                            .method = "alloc_for_extend",
                            .call_parameters = last_location_member_parameters,
                            .request_id_path = "batch.reqs[index].rid",
                            .hook = "sglang.srt.managers.schedule_batch.alloc_for_extend" },
        NativeRegionBinding{ .identity = "sglang.publish.output",
                            .semantic = "batch_output",
                            .module_type = "sglang.srt.managers.scheduler_components.output_streamer.SchedulerOutputStreamer",
                            .method = "stream_output",
                            .parameters = output_parameters,
                            .call_parameters = output_call_parameters,
                            .hook = "sglang.srt.managers.scheduler_components.output_streamer.SchedulerOutputStreamer.stream_output" },
        NativeRegionBinding{ .identity = "sglang.publish.request.prefill",
                            .semantic = "request_publication",
                            .module_type = "sglang.srt.managers.scheduler_components.batch_result_processor.SchedulerBatchResultProcessor",
                            .method = "process_batch_result_prefill",
                            .call_parameters = request_publication_parameters,
                            .request_id_path = "req.rid",
                            .hook =
                                 "sglang.srt.managers.scheduler_components.batch_result_processor.SchedulerBatchResultProcessor.process_batch_result_prefill" },
        NativeRegionBinding{ .identity = "sglang.publish.request.decode",
                            .semantic = "request_publication",
                            .module_type = "sglang.srt.managers.scheduler_components.batch_result_processor.SchedulerBatchResultProcessor",
                            .method = "process_batch_result_decode",
                            .call_parameters = request_publication_parameters,
                            .request_id_path = "req.rid",
                            .hook =
                                 "sglang.srt.managers.scheduler_components.batch_result_processor.SchedulerBatchResultProcessor.process_batch_result_decode" },
        NativeRegionBinding{ .identity = "sglang.batch.merge",
                            .semantic = "batch_merge",
                            .module_type = "sglang.srt.managers.schedule_batch.ScheduleBatch",
                            .method = "merge_batch",
                            .call_parameters = merge_parameters,
                            .hook = "sglang.srt.managers.schedule_batch.ScheduleBatch.merge_batch" },
        NativeRegionBinding{ .identity = "sglang.batch.filter",
                            .semantic = "batch_filter",
                            .module_type = "sglang.srt.managers.schedule_batch.ScheduleBatch",
                            .method = "filter_batch",
                            .call_parameters = filter_parameters,
                            .hook = "sglang.srt.managers.schedule_batch.ScheduleBatch.filter_batch" },
        NativeRegionBinding{ .identity = "sglang.chunk_cache.publish_prefix",
                            .semantic = "prefix_publication",
                            .module_type = "sglang.srt.mem_cache.chunk_cache.ChunkCache",
                            .method = "cache_unfinished_req",
                            .call_parameters = prefix_publication_parameters,
                            .component_path = "tree_cache",
                            .request_id_path = "req.rid" },
        NativeRegionBinding{ .identity = "sglang.sampling",
                            .semantic = "sampling",
                            .module_type = "sglang.srt.model_executor.model_runner.ModelRunner",
                            .method = "sample",
                            .inputs = metadata_inputs,
                            .call_parameters = sampling_parameters,
                            .component_path = ".",
                            .cpu_inputs = metadata_cpu_inputs,
                            .component_owner = "model_runner" },
        NativeRegionBinding{ .identity = "sglang.npu.attention.metadata",
                            .semantic = "attention_metadata",
                            .module_type = "sglang.srt.hardware_backend.npu.attention.ascend_backend.AscendAttnBackend",
                            .method = "init_forward_metadata",
                            .inputs = metadata_inputs,
                            .parameters = metadata_parameters,
                            .call_parameters = metadata_call_parameters,
                            .component_path = "attn_backend",
                            .cpu_inputs = metadata_cpu_inputs,
                            .component_owner = "model_runner" },
        NativeRegionBinding{ .identity = "sglang.npu.allocator.extend",
                            .semantic = "kv_allocation",
                            .module_type = "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator",
                            .method = "alloc_extend",
                            .parameters = allocation_parameters,
                            .call_parameters = extend_parameters,
                            .outputs = allocation_outputs,
                            .module_inputs = allocation_inputs,
                            .component_path = "token_to_kv_pool_allocator",
                            .cpu_inputs = extend_cpu_inputs },
        NativeRegionBinding{ .identity = "sglang.npu.allocator.decode",
                            .semantic = "kv_allocation",
                            .module_type = "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator",
                            .method = "alloc_decode",
                            .parameters = allocation_parameters,
                            .module_inputs = allocation_inputs,
                            .component_path = "token_to_kv_pool_allocator",
                            .cpu_inputs = decode_cpu_inputs },
        NativeRegionBinding{ .identity = "sglang.npu.allocator.free",
                            .semantic = "kv_release",
                            .module_type = "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator",
                            .method = "free",
                            .inputs = release_inputs,
                            .parameters = allocation_parameters,
                            .module_inputs = allocation_inputs,
                            .component_path = "token_to_kv_pool_allocator" },
        NativeRegionBinding{ .identity = "sglang.npu.allocator.free_group_begin",
                            .semantic = "kv_release_group",
                            .module_type = "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator",
                            .method = "free_group_begin",
                            .parameters = allocation_parameters,
                            .module_inputs = allocation_inputs,
                            .component_path = "token_to_kv_pool_allocator" },
        NativeRegionBinding{ .identity = "sglang.npu.allocator.free_group_end",
                            .semantic = "kv_release_group",
                            .module_type = "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator",
                            .method = "free_group_end",
                            .parameters = allocation_parameters,
                            .module_inputs = allocation_inputs,
                            .component_path = "token_to_kv_pool_allocator" },
        NativeRegionBinding{ .identity = "sglang.npu.allocator.merge_and_sort_free",
                            .semantic = "kv_page_merge",
                            .module_type = "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator",
                            .method = "merge_and_sort_free",
                            .parameters = allocation_parameters,
                            .module_inputs = allocation_inputs,
                            .component_path = "token_to_kv_pool_allocator" },
        NativeRegionBinding{ .identity = "sglang.parallel.all_reduce",
                            .semantic = "logical_collective",
                            .module_type = "sglang.srt.distributed.parallel_state.GroupCoordinator",
                            .method = "all_reduce",
                            .inputs = collective_inputs,
                            .parameters = collective_parameters,
                            .outputs = collective_outputs,
                            .hook = "sglang.srt.distributed.parallel_state.GroupCoordinator.all_reduce" },
        NativeRegionBinding{ .identity = "sglang.parallel.all_gather",
                            .semantic = "logical_collective",
                            .module_type = "sglang.srt.distributed.parallel_state.GroupCoordinator",
                            .method = "all_gather",
                            .inputs = collective_inputs,
                            .parameters = collective_parameters,
                            .call_parameters = gather_parameters,
                            .outputs = collective_outputs,
                            .hook = "sglang.srt.distributed.parallel_state.GroupCoordinator.all_gather" },
    };

    return values;
}

std::vector<NativeRegionBinding> SGLangBindings::regions() const {
    const auto serving = serving_regions();
    const auto model = Qwen3Regions::bindings();
    std::vector<NativeRegionBinding> result(serving.begin(), serving.end());
    result.insert(result.end(), model.begin(), model.end());
    return result;
}

const NativeRegionBinding & SGLangBindings::region(std::string_view identity) const {
    // Both groups own static declarations; references outlive extraction.
    for (const auto group : { serving_regions(), Qwen3Regions::bindings() }) {
        for (const auto & value : group) {
            if (value.identity == identity) return value;
        }
    }
    throw std::invalid_argument("unknown SGLang region binding: " + std::string(identity));
}

nlohmann::json SGLangBindings::describe_regions() const {
    auto bindings = nlohmann::json::array();
    for (const auto & value : regions()) {
        auto outputs = nlohmann::json::array();
        for (const auto & output : value.outputs)
            outputs.push_back({
                {  "name",                                                            output.name },
                { "index", output.index ? nlohmann::json(*output.index) : nlohmann::json(nullptr) }
            });
        bindings.push_back({
            {              "id",                                                                           value.identity },
            {            "step", value.component_path.empty() && value.hook.empty() ? "model_region" : "component_region" },
            {           "phase",                                                                                  nullptr },
            {        "semantic",                                                                           value.semantic },
            {     "module_type",                                                                        value.module_type },
            {          "method",                                                                             value.method },
            {        "cpu_name",                                                                         value.cpu_name() },
            {          "inputs",                                                                             value.inputs },
            {      "parameters",                                                    describe_parameters(value.parameters) },
            { "call_parameters",                                               describe_parameters(value.call_parameters) },
            {         "outputs",                                                                                  outputs },
            {   "module_inputs",                                                 describe_parameters(value.module_inputs) },
            {  "component_path",                                                                     value.component_path },
            { "component_owner",                                                                    value.component_owner },
            {      "cpu_inputs",                                                                         value.cpu_inputs },
            { "request_id_path",                                                                    value.request_id_path },
            {            "hook",                                                                               value.hook }
        });
    }

    auto parameters = nlohmann::json::array();
    for (const auto name : { "module_type", "method", "module_parameters", "call_parameters", "inputs", "outputs", "cpu_values", "request_id", "group_ranks" })
        parameters.push_back({
            { "name", name }
        });
    return {
        { "definition", { { "parameters", parameters } } },
        {   "bindings",                         bindings }
    };
}

} // namespace markov
