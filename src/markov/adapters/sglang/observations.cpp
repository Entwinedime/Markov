#include "markov/adapters/sglang/observations.hpp"

#include "markov/adapters/sglang/allocation.hpp"
#include "markov/adapters/sglang/cache_indices.hpp"
#include "markov/adapters/sglang/input_preparation.hpp"
#include "markov/adapters/sglang/result_output.hpp"

#include "markov/adapters/sglang/dense.hpp"
#include "markov/adapters/sglang/metadata.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace markov::adapters::sglang {

using namespace trace_graph::source;
namespace {

using namespace trace_graph::planning;

std::vector<ComputeObservation> read_compute(const ProbeEvent & event, const ComputeFrame & frame, const AttentionDescriptor * descriptor) {
    if (event.name == "runtime.phase.attention") {
        if (!descriptor->operation || frame.sequence_tokens.size() != 1 || !frame.query_tokens || frame.query_tokens->size() != 1) return {};
        std::vector<ComputeObservation> result;
        for (const auto & write : descriptor->cache_writes) {
            const auto rows =
                descriptor->tensor_query_tokens == frame.query_tokens->front() ? std::optional(RowBinding{ RowBasis::QueryTokens, 1 }) : std::nullopt;
            result.push_back({ write, RowInput{ descriptor->tensor_query_tokens }, rows });
        }
        result.push_back({
            *descriptor->operation,
            FrameInput{ frame.query_tokens->front(), descriptor->tensor_query_tokens, frame.sequence_tokens.front(), 1, descriptor->page_tokens },
            std::nullopt
        });
        return result;
    }

    return read_dense_work(event);
}

std::optional<std::vector<ComputeOperation>> attention_writes(const nlohmann::json & args) {
    if (!args.at("save_kv_cache").get<bool>() || !args.at("key_present").get<bool>() || !args.at("value_present").get<bool>())
        return std::vector<ComputeOperation>{};

    const auto & cache = args.at("cache_write");
    if (cache.at("pool_implementation") != "sglang.srt.hardware_backend.npu.memory_pool_npu.NPUMHATokenToKVPool" || cache.at("use_fia") != true
        || cache.at("cache_dtype") != "torch.bfloat16" || cache.at("key_dtype") != cache.at("cache_dtype") || cache.at("value_dtype") != cache.at("cache_dtype")
        || cache.at("location_dtype") != "torch.int64")
        return std::nullopt;

    std::vector<ComputeOperation> result;
    for (const auto * tensor : { "key", "value" }) {
        const auto shape = cache.at(std::string(tensor) + "_shape").get<std::vector<double>>();
        const auto stride = cache.at(std::string(tensor) + "_stride").get<std::vector<double>>();
        if (shape.size() != 3 || stride.size() != 3 || stride[2] != 1 || stride[1] != shape[2] || shape[0] != args.at("query_shape").at(0).get<double>()
            || cache.at("locations").get<double>() != shape[0])
            return std::nullopt;

        result.push_back({
            "ascend.scatter_nd_update_",
            CacheWriteGeometry{ "attention." + std::string(tensor) + "_write",
                               shape[1] * shape[2],
                               stride[0],
                               "ascend.slice.bfloat16.aicore", "ascend.scatter_nd_update.bfloat16.aicore" }
        });
    }
    return result;
}

} // namespace

void ProbeReader::observe(const ProbeEvent & event) {
    if (event.name == "prefetch.query" || event.name == "prefetch.issue" || event.name == "prefetch.check" || event.name == "prefetch.accept") {
        observe_prefetch_response(event);
        return;
    }
    if (event.name == "runtime.scheduler.request_broadcast") {
        const auto & args = event.args;
        if (args.at("status") != "returned") throw std::runtime_error("Cannot interpret a failed source request broadcast");
        if (args.at("empty").get<bool>()) {
            const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + event.process + ":" + event.thread);
            request_broadcasts_.emplace(
                BatchIdentity{
                    event.process,
                    args.at("batch_id").get<Index>()
            },
                RequestBroadcast{ { resource, event.begin_ns, event.end_ns, event.record },
                                  args.at("participant").get<Index>(),
                                  args.at("members").get<std::vector<Index>>() });
        }
        return;
    }
    if (event.name == "runtime.hicache.file.prefetch_issue" || event.name == "runtime.hicache.file.prefetch_progress"
        || event.name == "runtime.hicache.load_back" || event.name == "runtime.hicache.start_loading" || event.name == "runtime.hicache.load_failed") {
        observe_regenerated_control(event);
        return;
    }
    if (event.name.starts_with("storage.file.")) {
        observe_storage(event);
        return;
    }
    if (event.name.starts_with("region.")) {
        observe_response(event);
        return;
    }
    const auto & name = event.name;
    const auto & args = event.args;
    const auto & process = event.process;
    observe_result(event);
    if (name == "runtime.phase.batch_inputs" && args.at("status") == "returned") {
        const auto context = read_input_context(args);
        std::vector<Index> requests;
        for (const auto & identity : context.requests) requests.push_back(output_.name(identity));
        const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + process + ":" + event.thread);
        schedule_inputs_.emplace(std::move(requests),
                                 InputFacts{
                                     { resource, event.begin_ns, event.end_ns, event.record },
                                     context.phase,
                                     context.plain,
                                     context.deterministic_sampling
        });
        ++output_.counts[std::string("schedule_input_facts.") + (context.plain ? "plain" : "unsupported")];
    }
    if (name == "runtime.phase.batch_input_start") {
        ComputeFrame frame{ Phase::Prefill,
                            {},
                            args.at("sequence_tokens").get<std::vector<double>>(),
                            args.at("query_tokens").get<std::vector<double>>(),
                            args.at("page_size_tokens").get<double>(),
                            event.record,
                            {} };
        const auto & requests = args.at("request_ids");
        const auto & prompts = args.at("prompt_token_ids");
        if (requests.size() != prompts.size() || requests.size() != frame.sequence_tokens.size() || requests.size() != frame.query_tokens->size())
            throw std::runtime_error("Batch input dimensions do not match its requests");
        for (Index i = 0; i < requests.size(); ++i) {
            const auto identity = requests.at(i).get<std::string>();
            const auto tokens = prompts.at(i).get<std::vector<std::int64_t>>();
            output_.observe_tokens(identity, TokenRole::Prompt, output_.token_paths.intern(tokens), event.record);
            frame.requests.push_back(output_.name(identity));
        }
        forward_frames_[process] = output_.compute_frames.size();
        output_.compute_frames.push_back(std::move(frame));
    }
    if (name == "runtime.hicache.extend_allocator") {
        const auto frame = forward_frames_.find(process);
        if (frame == forward_frames_.end()) ++output_.counts["extend_allocator_without_forward_context"];
        else {
            const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + process + ":" + event.thread);
            output_.compute_frames[frame->second].preparations.push_back({
                { resource, event.begin_ns, event.end_ns, event.record },
                read_extend_allocator(args),
                "prefill_allocation"
            });
            input_windows_.insert_or_assign(process,
                                            InputWindow{
                                                { resource, event.begin_ns, event.end_ns, event.record },
                                                {},
                                                {}
            });
        }
    }
    if (name == "runtime.hicache.decode_allocator") {
        const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + process + ":" + event.thread);
        if (pending_decode_allocations_.contains(process)) ++output_.counts["unbound_decode_allocator"];
        pending_decode_allocations_.insert_or_assign(process,
                                                     ObservedCallSequence{
                                                         { resource, event.begin_ns, event.end_ns, event.record },
                                                         read_decode_allocator(args),
                                                         "decode_allocation"
        });
    }
    if (name == "runtime.hicache.decode_allocation") {
        ComputeFrame frame{ trace_graph::planning::Phase::Decode,
                            {},
                            args.at("sequence_lens_tokens").get<std::vector<double>>(),
                            {},
                            args.at("page_size_tokens").get<double>(),
                            event.record,
                            {} };
        for (const auto & identity : args.at("request_ids")) frame.requests.push_back(output_.name(identity.get<std::string>()));
        frame.query_tokens = std::vector<double>(frame.requests.size(), args.at("token_per_req").get<double>());
        const auto allocation = pending_decode_allocations_.find(process);
        if (allocation != pending_decode_allocations_.end() && event.begin_ns <= allocation->second.region.begin_ns
            && allocation->second.region.end_ns <= event.end_ns && args.at("token_per_req") == 1) {
            // The inner allocator record is emitted before its enclosing call.
            input_windows_.insert_or_assign(process, InputWindow{ allocation->second.region, {}, {} });
            frame.preparations.push_back(std::move(allocation->second));
            pending_decode_allocations_.erase(allocation);
        }
        else {
            const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + process + ":" + event.thread);
            frame.preparations.push_back({
                { resource, event.begin_ns, event.end_ns, event.record },
                nullptr,
                "decode_allocation"
            });
            ++output_.counts["decode_allocator_without_matching_call"];
        }
        forward_frames_[process] = output_.compute_frames.size();
        output_.compute_frames.push_back(std::move(frame));
    }

    if ((name == "runtime.hicache.extend_allocation" || name == "runtime.hicache.decode_allocation") && args.at("status") == "returned") {
        const auto window = input_windows_.find(process);
        if (window != input_windows_.end()) {
            const auto & allocator = window->second.allocator;
            window->second.table_write = ObservedRegion{ allocator.resource, allocator.end_ns, event.end_ns, event.record };
        }
    }
    if ((name == "runtime.phase.input_buffers" && args.at("status") == "returned") || name == "runtime.phase.forward_input_boundary") {
        const auto window = input_windows_.find(process);
        if (window != input_windows_.end() && window->second.table_write) {
            const auto & table = *window->second.table_write;
            window->second.forward = ObservedRegion{ table.resource, table.end_ns, event.begin_ns, event.record };
        }
    }

    if (name == "hicache_cache_lookup_input_end" || name == "hicache_cache_finished_req_lifecycle_commit_end") {
        const auto path = token_path(args.at("token_dictionary"));
        const auto role = name == "hicache_cache_lookup_input_end" ? TokenRole::LookupInput : TokenRole::Committed;
        output_.observe_tokens(args.at("request_id").get<std::string>(), role, path, event.record);
        if (role == TokenRole::LookupInput)
            lookups_.push_back({
                output_.name(args.at("request_id").get<std::string>()),
                { output_.resource(ResourceKind::CpuThread, "cpu:" + event.process + ":" + event.thread), event.begin_ns, event.end_ns, event.record }
            });
    }
    if (name == "hicache_cache_extend_input_start") {
        const auto & dictionaries = args.at("token_dictionaries");
        const auto & identities = args.at("request_ids");
        ComputeFrame frame{ trace_graph::planning::Phase::Prefill, {}, {}, std::nullopt, std::nullopt, event.record, {} };
        for (std::size_t i = 0; i < identities.size(); ++i) {
            const auto path = token_path(dictionaries.at(i));
            const auto request = identities[i].get<std::string>();
            output_.observe_tokens(request, TokenRole::Prompt, path, event.record);
            frame.requests.push_back(output_.name(request));
            frame.sequence_tokens.push_back(output_.token_paths.length(path));
        }
        forward_frames_[process] = output_.compute_frames.size();
        output_.compute_frames.push_back(std::move(frame));
    }

    if (name == "runtime.phase.attention_metadata" && args.at("status") == "returned") {
        const auto frame = forward_frames_.find(process);
        if (frame == forward_frames_.end()) ++output_.counts["metadata_without_forward_context"];
        else {
            const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + process + ":" + event.thread);
            auto & preparations = output_.compute_frames[frame->second].preparations;
            const auto window = input_windows_.find(process);
            if (window != input_windows_.end() && window->second.table_write && window->second.forward) {
                preparations.push_back({ *window->second.table_write, read_request_table_write(args), "request_table_write" });
                pending_inputs_.push_back({ frame->second, preparations.size(), args });
                preparations.push_back({ *window->second.forward, nullptr, "forward_inputs" });
                input_windows_.erase(window);
            }
            else ++output_.counts["input_preparation_boundaries_unavailable"];
            sequence_dtypes_[process] = args.at("sequence_dtype").get<std::string>();
            preparations.push_back({
                { resource, event.begin_ns, event.end_ns, event.record },
                read_metadata(args),
                "attention_metadata"
            });
        }
    }

    if (name == "runtime.phase.embedding" && args.at("status") == "returned") embedding_windows_[process] = { event.end_ns, 0 };
    if (name == "runtime.phase.device_collective" && args.at("dense_role") == "input_embedding" && args.at("status") == "returned")
        embedding_windows_.at(process).reduction_begin = event.begin_ns;
    if (name == "runtime.phase.embedding_forward" && args.at("status") == "returned" && args.at("tp_size").get<int>() > 1) {
        const auto window = embedding_windows_.at(process);
        if (window.lookup_end < event.begin_ns || window.reduction_begin <= window.lookup_end || window.reduction_begin > event.end_ns)
            throw std::runtime_error("Embedding mask has no bounded native lookup-to-reduction window");

        auto mask = event;
        mask.begin_ns = window.lookup_end;
        mask.end_ns = window.reduction_begin;
        observe_compute(mask);
    }

    constexpr std::array<std::string_view, 15> compute_roles{
        "runtime.phase.linear",           "runtime.phase.logits_projection", "runtime.phase.normalization",
        "runtime.phase.gated_activation", "runtime.phase.attention",         "runtime.phase.input_buffers",
        "runtime.phase.embedding_mask",   "runtime.phase.embedding",         "runtime.phase.qk_reshape",
        "runtime.phase.rotary",           "runtime.phase.rotary_table",      "runtime.phase.qkv_norm_rope",
        "runtime.phase.logits_output",    "runtime.phase.logits_pruning",    "runtime.phase.greedy_sampling"
    };
    if (name == "runtime.phase.logits_output" && args.at("status") == "returned") logits_output_ends_[process] = event.end_ns;
    if (name == "runtime.phase.greedy_sampling" && args.at("status") == "returned") {
        const auto frame = forward_frames_.find(process);
        if (frame != forward_frames_.end()) sampling_ends_[frame->second] = event.end_ns;
        if (frame != forward_frames_.end() && output_.compute_frames[frame->second].phase == trace_graph::planning::Phase::Prefill) {
            const auto before = logits_output_ends_.find(process);
            if (before != logits_output_ends_.end()) {
                const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + process + ":" + event.thread);
                output_.compute_frames[frame->second].preparations.push_back({
                    { resource, before->second, event.begin_ns, event.record },
                    sampling_positions(sequence_dtypes_.at(process)),
                    "sampling_positions"
                });
            }
            else ++output_.counts["sampling_position_boundary_unavailable"];
        }
    }
    if (std::ranges::find(compute_roles, name) != compute_roles.end()) observe_compute(event);
    if (name == "runtime.phase.device_collective") observe_collective(event);
    observe_control(event);
}

void ProbeReader::finish() {
    finish_prefetch_responses();
    for (const auto & collective : output_.collectives) {
        auto & frame = output_.compute_frames[collective.frame];
        if (frame.participant && *frame.participant != collective.rank)
            throw std::runtime_error("One source frame contains conflicting collective participants");
        frame.participant = collective.rank;
    }
    std::map<Index, std::vector<std::pair<Index, const RequestBroadcast *>>> broadcast_groups;
    for (const auto & [identity, call] : request_broadcasts_) {
        const auto result = batch_results_.find(identity);
        if (result == batch_results_.end()) continue;
        broadcast_groups[identity.second].emplace_back(output_.batch_results[result->second].frame, &call);
    }
    for (const auto & [identity, calls] : broadcast_groups) {
        const auto & members = calls.front().second->members;
        std::set<Index> participants;
        Time all_entered = calls.front().second->region.begin_ns;
        for (const auto & [frame, call] : calls) {
            const auto & observed = output_.compute_frames[frame];
            if (call->members != members || observed.requests != output_.compute_frames[calls.front().first].requests
                || observed.participant != call->participant)
                throw std::runtime_error("Request broadcast and compute participants disagree");
            participants.insert(call->participant);
            all_entered = std::max(all_entered, call->region.begin_ns);
        }
        if (participants != std::set<Index>(members.begin(), members.end()) || participants.size() != calls.size()) {
            ++output_.counts["request_broadcast_participants_incomplete"];
            continue;
        }
        if (std::ranges::any_of(calls, [&](const auto & value) { return value.second->region.end_ns < all_entered; })) {
            ++output_.counts["request_broadcast_without_common_response"];
            continue;
        }
        for (const auto & [frame, call] : calls) {
            output_.compute_frames[frame].preparation_rendezvous = CallerRendezvous{ call->region, all_entered };
            ++output_.counts["request_broadcast_rendezvous"];
        }
    }
    request_broadcasts_.clear();
    output_.counts["result_copy_without_batch_run"] += result_copies_.size();
    output_.counts["result_send_without_processing"] += result_sends_.size();
    result_copies_.clear();
    result_sends_.clear();
    // Sidecars may follow the main probe file; qualify after every input is read.
    std::vector<const BatchResult *> results(output_.compute_frames.size(), nullptr);
    for (const auto & result : output_.batch_results) results.at(result.frame) = &result;
    for (auto & frame : output_.compute_frames) {
        const auto result = results[&frame - output_.compute_frames.data()];
        if (!result) continue;
        const auto [begin, end] = schedule_inputs_.equal_range(frame.requests);
        for (auto at = begin; at != end; ++at) {
            const auto & fact = at->second;
            if (fact.phase != frame.phase || fact.region.resource != result->run.resource || fact.region.end_ns > result->run.begin_ns) continue;
            if (!frame.preparation_region || frame.preparation_region->end_ns < fact.region.end_ns) frame.preparation_region = fact.region;
        }
    }
    for (const auto & pending : pending_inputs_) {
        auto & frame = output_.compute_frames[pending.frame];
        auto & preparation = frame.preparations[pending.preparation];
        const auto & region = preparation.region;
        std::vector<const InputFacts *> matching;
        const auto [begin, end] = schedule_inputs_.equal_range(frame.requests);
        for (auto at = begin; at != end; ++at) {
            const auto & fact = at->second;
            const auto & observed = fact.region;
            if (fact.phase == frame.phase && observed.resource == region.resource && observed.begin_ns <= region.begin_ns && region.begin_ns < observed.end_ns
                && observed.end_ns <= region.end_ns)
                matching.push_back(&fact);
        }
        auto context = InputContextStatus::Unobserved;
        if (!matching.empty()) {
            const bool plain = matching.size() == 1 && matching.front()->plain;
            context = plain ? InputContextStatus::PlainText : InputContextStatus::Unsupported;
            ++output_.counts[plain ? "forward_entry_context_supported" : "forward_entry_context_unsupported_or_incomplete"];
        }
        const auto sampling = matching.size() == 1 ? std::optional(matching.front()->deterministic_sampling) : deterministic_sampling_;
        if (sampling) preparation.definition = read_forward_inputs(pending.metadata, *sampling, context);
        const auto result = results[pending.frame];
        const auto sampled = sampling_ends_.find(pending.frame);
        if (result && result->copy && sampled != sampling_ends_.end() && sampled->second <= result->run.end_ns) {
            frame.preparations.push_back({
                { result->run.resource, sampled->second, result->run.end_ns, result->run.record },
                read_result_output(pending.metadata, *result->copy),
                "result_output"
            });
        }
    }
    for (auto & result : output_.batch_results) {
        if (!result.processed) continue;
        const auto input = std::ranges::find_if(pending_inputs_, [&](const auto & value) { return value.frame == result.frame; });
        result.processed->callers = read_result_callers(output_, result, input == pending_inputs_.end() ? nullptr : &input->metadata);
        ++output_.counts[result.processed->callers.empty() ? "result_callers_unavailable" : "result_callers_supported"];
    }

    for (const auto & lookup : lookups_) {
        for (auto & result : output_.batch_results) {
            auto & frame = output_.compute_frames[result.frame];
            if (frame.phase != Phase::Prefill || frame.requests != std::vector<Index>{ lookup.request } || result.run.resource != lookup.region.resource
                || result.run.begin_ns < lookup.region.end_ns)
                continue;
            frame.lookups = read_cache_lookup(output_, lookup.region);
            ++output_.counts[frame.lookups.empty() ? "cache_lookup_recipe_unavailable" : "cache_lookup_recipe_supported"];
            break;
        }
    }
    lookups_.clear();
    pending_inputs_.clear();
    schedule_inputs_.clear();
}

void ProbeReader::observe_result(const ProbeEvent & event) {
    const auto & args = event.args;
    const bool run = event.name == "runtime.request.batch_run";
    const bool copy = event.name == "runtime.response.copy_to_cpu";
    const bool processed = event.name == "runtime.response.process_result";
    const bool sent = event.name == "runtime.response.scheduler_send";
    if ((!run && !copy && !processed && !sent) || args.value("status", "returned") != "returned") return;
    if (!args.contains("batch_id") || args.at("batch_id").is_null()) {
        if (!sent) ++output_.counts["result_without_batch_identity"];
        return;
    }

    const BatchIdentity identity{ event.process, args.at("batch_id").get<Index>() };
    const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + event.process + ":" + event.thread);
    const ObservedRegion region{ resource, event.begin_ns, event.end_ns, event.record };
    // These are semantic ports, not additional parent CPU service charges.
    const auto name = output_.name(event.name);
    output_.point({ event.begin_ns, resource, name, event.record });
    output_.point({ event.end_ns, resource, name, event.record });
    if (copy) {
        ResultCopy value{ region,
                          args.at("token_shape").get<std::vector<Index>>(),
                          args.at("token_dtype").get<std::string>(),
                          args.at("token_device").get<std::string>(),
                          args.at("token_bytes").get<Index>(),
                          args.at("return_logprob").get<bool>() || args.at("return_hidden_states").get<bool>() || args.at("additional_outputs").get<bool>() };
        const auto batch = batch_results_.find(identity);
        if (batch != batch_results_.end()) {
            auto & target = output_.batch_results[batch->second].copy;
            if (target) throw std::runtime_error("Duplicate result copy for one batch");
            target = std::move(value);
        }
        else if (!result_copies_.emplace(identity, std::move(value)).second) throw std::runtime_error("Duplicate result copy for one batch");
        return;
    }
    if (sent) {
        std::vector<Index> requests;
        for (const auto & request : args.at("request_ids")) requests.push_back(output_.name(request.get<std::string>()));
        result_sends_[identity].push_back({ region, std::move(requests) });
        return;
    }
    if (run) {
        const auto frame = forward_frames_.find(event.process);
        if (frame == forward_frames_.end() || args.at("phase") == "other") {
            ++output_.counts["result_without_compute_frame"];
            return;
        }
        const auto & observed = output_.compute_frames[frame->second];
        const auto requests = args.at("request_ids").get<std::vector<std::string>>();
        if (observed.requests.size() != requests.size()) throw std::runtime_error("Result batch and compute frame have different requests");
        for (Index id = 0; id < requests.size(); ++id)
            if (output_.names[observed.requests[id]] != requests[id]) throw std::runtime_error("Result batch and compute frame identities disagree");

        BatchResult value{ frame->second, region, args.at("overlap").get<bool>(), {}, {}, {} };
        const auto copied = result_copies_.find(identity);
        if (copied != result_copies_.end()) {
            value.copy = std::move(copied->second);
            result_copies_.erase(copied);
        }
        if (!batch_results_.emplace(identity, output_.batch_results.size()).second) throw std::runtime_error("Duplicate result batch identity");
        output_.batch_results.push_back(std::move(value));
        return;
    }

    const auto batch = batch_results_.find(identity);
    if (batch == batch_results_.end()) {
        ++output_.counts["processed_result_without_batch_run"];
        return;
    }
    auto & value = output_.batch_results[batch->second];
    const auto & requests = output_.compute_frames[value.frame].requests;
    const auto identities = args.at("request_ids").get<std::vector<std::string>>();
    ProcessedResult result{ region,
                            args.at("output_tokens_before").get<std::vector<Index>>(),
                            args.at("output_tokens_after").get<std::vector<Index>>(),
                            args.at("finished_before").get<std::vector<bool>>(),
                            args.at("finished_after").get<std::vector<bool>>() };
    if (value.processed || identities.size() != requests.size() || result.output_tokens_before.size() != requests.size()
        || result.output_tokens_after.size() != requests.size() || result.finished_before.size() != requests.size()
        || result.finished_after.size() != requests.size())
        throw std::runtime_error("Processed result dimensions or identity are invalid");
    for (Index id = 0; id < requests.size(); ++id)
        if (output_.names[requests[id]] != identities[id] || result.output_tokens_after[id] < result.output_tokens_before[id])
            throw std::runtime_error("Processed result changed request order or removed output tokens");

    auto & controls = pending_controls_[resource];
    for (const auto id : controls) {
        const auto & control = output_.host_controls[id].region;
        if (region.begin_ns <= control.begin_ns && control.end_ns <= region.end_ns) result.controls.push_back(id);
    }
    std::erase_if(controls, [&](Index id) { return output_.host_controls[id].region.end_ns <= region.end_ns; });

    value.processed = std::move(result);
    const auto final_send = result_sends_.find(identity);
    if (final_send != result_sends_.end()) {
        value.final_sends = std::move(final_send->second);
        result_sends_.erase(final_send);
    }
}

void ProbeReader::observe_regenerated_control(const ProbeEvent & event) {
    const auto & args = event.args;
    if (args.at("status") != "returned") throw std::runtime_error("Cannot normalize a failed source cache control");

    const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + event.process + ":" + event.thread);
    ObservedRegion region{ resource, event.begin_ns, event.end_ns, event.record };
    std::string operation;
    if (event.name == "runtime.hicache.file.prefetch_issue") operation = "cache.prefetch.issue";
    else if (event.name == "runtime.hicache.file.prefetch_progress") {
        const auto key = std::pair(resource, output_.name(args.at("request_id").get<std::string>()));
        if (!args.at("ready").get<bool>()) {
            prefetch_waits_.try_emplace(key, region);
            return;
        }
        const auto waiting = prefetch_waits_.find(key);
        if (waiting != prefetch_waits_.end()) {
            auto loop = waiting->second;
            loop.end_ns = event.begin_ns;
            output_.regenerated_controls.push_back(loop);
            ++output_.counts["regenerated_control.scheduler.idle_turn"];
            prefetch_waits_.erase(waiting);
        }
        // The final ready call is replaced by check and acceptance responses;
        // a revoked query also has an explicit target path, never a zero wait.
        operation = "cache.prefetch.check";
    }
    else if (event.name == "runtime.hicache.load_back" && args.at("loaded_tokens").get<std::size_t>() > 0) operation = "cache.load.prepare";
    else if (event.name == "runtime.hicache.load_failed") operation = "cache.load.failed";
    else if (event.name == "runtime.hicache.start_loading" && args.at("producer").get<std::int64_t>() >= 0) operation = "cache.load.caller";
    else return;

    ++output_.counts["regenerated_control." + operation];
    output_.regenerated_controls.push_back(region);
}

void ProbeReader::observe_control(const ProbeEvent & event) {
    const auto & args = event.args;
    if (args.value("status", "returned") != "returned") return;
    trace_graph::planning::Features variables;
    std::string operation;
    if (event.name == "runtime.hicache.cache_unfinished") {
        operation = "cache.update_prefix";
        for (const auto * field : { "page_size_tokens", "input_tokens", "protected_tokens_before", "protected_tokens_after" })
            variables[field] = args.at(field).get<double>();
        variables["chunked"] = args.at("chunked").get<bool>() ? 1 : 0;
    }
    else if (event.name == "runtime.hicache.match_segments") {
        operation = "cache.match_segments";
        for (const auto * field : { "input_tensors", "elements", "element_bytes", "tensor_rank" }) variables[field] = args.at(field).get<double>();
        variables["device_npu"] = args.at("device_type") == "npu" ? 1 : 0;
    }
    else if (event.name == "hicache_cache_finished_req_lifecycle_commit_end" && args.contains("page_size_tokens") && args.contains("thread_cpu_ns")) {
        operation = "cache.commit_prefix";
        variables["page_size_tokens"] = args.at("page_size_tokens").get<double>();
        variables["insert_requested"] = args.at("insert_requested").get<bool>() ? 1 : 0;
        variables["committed_tokens"] = output_.token_paths.length(output_.request_tokens.at(args.at("request_id").get<std::string>()).committed.value().path);
    }
    else if (event.name == "runtime.hicache.page_release" || event.name == "runtime.hicache.release_group") {
        operation = event.name == "runtime.hicache.page_release" ? "allocator.release_pages" : "allocator.finish_release_group";
        for (const auto * field : { "page_size_tokens", "free_pages", "release_pages", "free_pages_after", "release_pages_after" })
            variables[field] = args.at(field).get<double>();
        variables["need_sort"] = args.at("allocator_need_sort").get<bool>() ? 1 : 0;
        variables["grouped"] = args.at("grouped").get<bool>() ? 1 : 0;
        if (event.name == "runtime.hicache.page_release") variables["released_tokens"] = args.at("released_tokens").get<double>();
        else {
            variables["grouped_tokens"] = args.at("grouped_tokens").get<double>();
            variables["grouped_tensors"] = args.at("grouped_tensors").get<double>();
        }
    }
    else return;

    const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + event.process + ":" + event.thread);
    pending_controls_[resource].push_back(output_.host_controls.size());
    const auto implementation =
        args.contains("allocator_implementation") ? args.at("allocator_implementation").get<std::string>() : args.at("cache_implementation").get<std::string>();
    output_.host_controls.push_back({
        { resource, event.begin_ns, event.end_ns, event.record },
        { { operation, implementation, "observed_control_envelope" }, std::move(variables) },
        args.at("thread_cpu_ns").get<Time>(),
        args.contains("request_id") ? std::optional(output_.name(args.at("request_id").get<std::string>())) : std::nullopt
    });
}

void ProbeReader::observe_collective(const ProbeEvent & event) {
    using namespace trace_graph::planning;
    const auto & args = event.args;
    if (args.at("status") != "returned") return;
    const auto frame = forward_frames_.find(event.process);
    if (frame == forward_frames_.end()) {
        ++output_.counts["collective_without_forward_context"];
        return;
    }

    const auto implementation = args.at("implementation").get<std::string>();
    const bool reduction = implementation == "sglang.NpuCommunicator.all_reduce";
    const bool gather = implementation == "sglang.NpuCommunicator.all_gather";
    const auto shape = args.at("input_shape").get<std::vector<double>>();
    const auto stride = args.at("input_stride").get<std::vector<double>>();
    if ((!reduction && !gather) || shape.size() != 2 || stride.size() != 2 || stride[0] != shape[1] || stride[1] != 1 || args.at("input_device_type") != "npu"
        || (reduction && args.at("operation") != "sum") || (gather && args.at("gather_dim") != 1 && args.at("gather_dim") != -1)) {
        ++output_.counts["collective_unsupported_implementation_or_layout"];
        return;
    }

    const auto members = args.at("group_members").get<std::vector<Index>>();
    const auto rank = args.at("global_rank").get<std::int64_t>();
    const auto sequence = args.at(reduction ? "observed_all_reduce_index" : "observed_all_gather_index").get<std::int64_t>();
    if (rank < 0 || sequence < 0 || members.empty() || members.size() != args.at("world_size").get<Index>()
        || std::ranges::count(members, static_cast<Index>(rank)) != 1
        || std::ranges::any_of(args.at("group_members"), [](const auto & member) { return member.template get<std::int64_t>() < 0; }))
        throw std::runtime_error("Collective membership or sequence is invalid");

    const auto identity = args.at("group").get<std::string>();
    const auto [group, inserted] = collective_groups_.try_emplace(std::pair(identity, members), output_.collective_groups.size());
    if (inserted) output_.collective_groups.push_back({ identity, members });
    const auto phase = output_.compute_frames[frame->second].phase == Phase::Prefill ? "prefill." : "decode.";
    const auto operation = reduction ? std::string(phase) + "all_reduce" : "collective.all_gather";
    const auto bytes = args.at("element_bytes").get<double>();
    const auto basis = args.value("last_token_projection", false) ? RowBasis::BatchSize : RowBasis::QueryTokens;
    const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + event.process + ":" + event.thread);
    output_.collectives.push_back({
        group->second,
        static_cast<Index>(rank),
        static_cast<Index>(sequence),
        frame->second,
        resource,
        event.begin_ns,
        event.end_ns,
        event.record,
        { { operation, implementation + ":" + args.at("input_dtype").get<std::string>(), "collective_completion_interval" },
                 { { "rows", shape[0] },
                 { "width", shape[1] },
                 { "element_bytes", bytes },
                 { "participants", static_cast<double>(members.size()) },
                 { "logical_tensor_bytes", shape[0] * shape[1] * bytes } }                                                                                                  },
        {                                                                                                             basis, args.at("rows_per_token").get<double>() }
    });
}

trace_graph::planning::PrefixId ProbeReader::token_path(const nlohmann::json & dictionary) {
    const auto identity = dictionary.at("token_path_id").get<std::string>();
    if (!dictionary.contains("token_ids")) return token_descriptors_.at(identity);

    // Native probes declare each payload once, then refer to its dictionary ID.
    // Resolve that protocol here; target identity uses the actual token ancestry.
    const auto tokens = dictionary.at("token_ids").get<std::vector<std::int64_t>>();
    const auto path = output_.token_paths.intern(tokens);
    const auto [entry, inserted] = token_descriptors_.try_emplace(identity, path);
    if (!inserted && entry->second != path) throw std::runtime_error("Source token dictionary reuses an ID for different content");
    return path;
}

const AttentionDescriptor & ProbeReader::attention_descriptor(const ProbeEvent & event, trace_graph::planning::Phase phase) {
    using namespace trace_graph::planning;
    const auto & args = event.args;
    const auto key = std::tuple(event.process, phase, args.at("descriptor_id").get<Index>());
    if (!args.at("descriptor_present").get<bool>()) return attention_descriptors_.at(key);

    AttentionDescriptor result{
        std::nullopt, {}, args.at("page_size_tokens").get<double>(), args.at("query_shape").at(0).get<double>(), std::nullopt, args.at("layer_id").get<Index>()
    };
    if (!args.at("extend_query_lengths").is_null()) result.query_tokens = args.at("extend_query_lengths").get<std::vector<double>>();
    const auto d = args.at("query_key_head_dim").get<double>(), v = args.at("value_head_dim").get<double>();
    // These are the eager Ascend FIA branches; graph, sliding-window and
    // grouped requests need their own execution and work descriptions.
    const bool supported =
        args.at("use_fia").get<bool>() && !args.at("use_mla").get<bool>() && args.at("use_alibi") != true && !args.at("graph_mode").get<bool>()
        && !args.at("cross_attention").get<bool>() && !args.at("encoder_present").get<bool>() && !args.at("sinks_present").get<bool>()
        && !args.at("mode_is_target_verify").get<bool>() && !args.at("mode_is_draft_extend").get<bool>() && args.at("sliding_window_size").get<int>() == -1
        && args.at("attention_cp_size").get<int>() == 1 && args.at("batch_size").get<int>() == 1 && args.at("value_heads") == args.at("key_heads")
        && (phase == Phase::Prefill ? ((d == v && (d == 128 || d == 192 || d == 256)) || (d == 192 && v == 128))
                                    : args.at("fia_torch_ops_dispatch") == "batch_invariant");
    const auto writes = attention_writes(args);
    if (supported && writes) {
        const auto implementation = phase == Phase::Prefill ? "ascend.fia.TND" : "ascend.fia.batch_invariant.BSND";
        result.operation = ComputeOperation{
            implementation,
            AttentionGeometry{ args.at("query_heads").get<double>(), args.at("key_heads").get<double>(), d, v, args.at("causal_attention").get<bool>() }
        };
        result.cache_writes = *writes;
    }
    const auto [stored, inserted] = attention_descriptors_.try_emplace(key, result);
    if (!inserted && stored->second != result) throw std::runtime_error("Attention descriptor ID is redefined with different work");
    return stored->second;
}

void ProbeReader::observe_compute(const ProbeEvent & event) {
    const auto & args = event.args;
    if (args.at("status") != "returned") return;
    if (event.name == "runtime.phase.logits_pruning" && args.at("passthrough").get<bool>()) {
        ++output_.counts["compute_view_only.logits_pruning"];
        return;
    }
    const auto context = forward_frames_.find(event.process);
    if (context == forward_frames_.end()) {
        ++output_.counts["compute_work_without_forward_context"];
        return;
    }
    auto & frame = output_.compute_frames[context->second];
    const AttentionDescriptor * descriptor = nullptr;
    if (event.name == "runtime.phase.attention") {
        const auto phase = frame.phase == trace_graph::planning::Phase::Prefill ? "prefill" : "decode";
        if (args.at("phase") != phase) {
            ++output_.counts["compute_frame_phase_mismatch"];
            return;
        }
        descriptor = &attention_descriptor(event, frame.phase);
        frame.page_tokens = descriptor->page_tokens;
        if (frame.phase == trace_graph::planning::Phase::Prefill) frame.query_tokens = descriptor->query_tokens;
    }

    auto observed = read_compute(event, frame, descriptor);
    if (observed.empty()) {
        ++output_.counts["compute_work_unsupported_geometry." + event.name];
        return;
    }

    // The probe supplies geometry and provenance, not a device measurement.
    // The independent provider estimates service; this CPU envelope is not charged.
    const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + event.process + ":" + event.thread);
    std::optional<Index> layer;
    if (descriptor) layer = descriptor->layer;
    else if (args.contains("model_layer_index") && !args.at("model_layer_index").is_null()) layer = args.at("model_layer_index").get<Index>();
    else if (args.contains("layer_id") && !args.at("layer_id").is_null()) layer = args.at("layer_id").get<Index>();
    std::vector<trace_graph::planning::Work> work;
    std::vector<trace_graph::planning::ComputePart> recipe;
    for (auto & part : observed) {
        for (auto & operation : part.operation.describe(frame.phase, part.input)) work.push_back(std::move(operation));
        recipe.push_back({ std::move(part.operation), part.rows });
    }
    const auto caller = event.name + ":" + args.value("implementation", std::string{});
    auto calls = read_dense_calls(event, recipe);
    const auto description = output_.describe_work(std::move(work), std::move(recipe), caller, std::move(calls.parameters));
    auto & definition = output_.work_descriptions[description];
    if (!definition.calls) definition.calls = std::move(calls.definition);
    output_.work_observations.push_back({ description, resource, event.begin_ns, event.end_ns, event.record, context->second, layer });
}

} // namespace markov::adapters::sglang
