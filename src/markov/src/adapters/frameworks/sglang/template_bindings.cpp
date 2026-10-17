#include "markov/adapters/frameworks/sglang/template_bindings.hpp"

#include "markov/adapters/frameworks/sglang/copy_layout.hpp"

#include "markov/contracts/json_input.hpp"

#include "markov/adapters/frameworks/sglang/batch_layout.hpp"
#include "markov/adapters/frameworks/sglang/bindings.hpp"
#include "markov/adapters/frameworks/sglang/models/attention_kernel_layout.hpp"
#include "markov/adapters/frameworks/sglang/models/model_kernel_layout.hpp"
#include "markov/adapters/frameworks/sglang/paged_kv_layout.hpp"
#include "markov/adapters/frameworks/sglang/request_pool_layout.hpp"
#include "markov/adapters/runtimes/ascend/hccl_mesh_layout.hpp"
#include "markov/graph/kernel_shapes.hpp"
#include "markov/templates/templates.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace markov {

void SGLangTemplateBindings::bind_membership(const nlohmann::json & source_state) {
    if (definition_.step != StepKind::Schedule) return;

    const auto & recorded_filters = source_state.at("batch_filters");
    for (const auto & filter : recorded_filters) {
        const auto before = filter.at("before").get<std::vector<std::string>>();
        BatchFilter selection{ before.size(), {} };
        for (const auto & request : filter.at("after")) {
            const auto position = std::find(before.begin(), before.end(), request.get<std::string>());
            if (position == before.end()) throw std::invalid_argument("native batch filter introduced a request");
            selection.retained_indices.push_back(static_cast<std::size_t>(position - before.begin()));
        }
        definition_.filters.push_back(std::move(selection));
    }

    // On the fixed non-overlap decode path, prefill filtering precedes
    // running filtering. The latter's input preserves left+right order.
    if (definition_.phase == BatchPhase::Decode && recorded_filters.size() == 2) {
        const auto & appended = recorded_filters[0].at("after");
        const auto & combined = recorded_filters[1].at("before");
        if (!appended.empty() && combined.size() > appended.size() && std::equal(appended.rbegin(), appended.rend(), combined.rbegin()))
            definition_.merge = BatchMerge{ combined.size() - appended.size(), appended.size() };
    }
}

void SGLangTemplateBindings::bind_publication(const nlohmann::json * published_requests, const nlohmann::json & source_state) {
    if (definition_.step == StepKind::Publish) {
        const auto policy = source_state.find("output_policy");
        if (policy != source_state.end()) {
            if (!policy->at("non_streaming").get<bool>())
                throw std::invalid_argument("native streaming output is outside the supported non-streaming serving scope");
            definition_.output_policy = PeriodicOutputPolicy{ input::positive(policy->at("force_interval_tokens")) };
        }
    }

    std::map<std::string, bool> observations;
    if (published_requests) {
        for (const auto & request : *published_requests) observations.emplace(request.at("request_id").get<std::string>(), request.at("finished").get<bool>());
    }

    // Native identity is resolved only here; consumers use ordered target slots.
    for (const auto & request : definition_.source_requests) {
        const auto found = observations.find(request.id);
        definition_.publication_finishes.push_back(found == observations.end() ? std::nullopt : std::optional(found->second));
    }
}

void SGLangTemplateBindings::bind_kv(std::uint32_t page_size_tokens) {
    std::optional<std::vector<SourceRequest>> finished;
    if (definition_.step == StepKind::Publish && definition_.phase == BatchPhase::Decode && definition_.source_batch) {
        finished.emplace();
        for (std::size_t slot = 0; slot < definition_.source_requests.size(); ++slot) {
            const auto observed = definition_.publication_finishes[slot];
            if (!observed) {
                finished.reset();
                break;
            }
            if (*observed) finished->push_back(definition_.source_requests[slot]);
        }
    }


    for (const auto & node : definition_.graph) {
        const auto & node_observation = definition_.observation(node);
        if (!node_observation.kernel) continue;
        auto layout = PagedKvLayout::bind(definition_, node, finished, page_size_tokens);
        if (!layout) definition_.kv_kernel_layouts.emplace(node_observation.source_span, std::unexpected(layout.error()));
        else if (*layout) definition_.kv_kernel_layouts.emplace(node_observation.source_span, std::move(**layout));
    }

    if (finished) {
        if (finished->size() == 1) {
            const auto & request = finished->front();
            bind_release_transfer(static_cast<std::uint64_t>(request.context_tokens) + request.input_tokens);
        }
        for (std::size_t index = 0; index < definition_.regions.size(); ++index) {
            const auto & region = definition_.regions[index];
            if (region.binding == "sglang.npu.allocator.free_group_end" && region.parent_call == definition_.source_call) {
                // In the supported non-overlap path, finished requests populate
                // this group. An empty group performs no page release.
                const auto count = finished->size();
                const auto minimum = count >= 2 ? 2 : count;
                const auto maximum = count >= 2 ? PagedKvLayout::release_concat_limit : count;
                if (count > maximum) continue;
                definition_.kv_region = KvRegionBranch{ KvRegionKind::ReleaseGroup, index, count != 0, minimum, maximum };
            }
        }
    }

    if (definition_.step != StepKind::Prepare || definition_.phase != BatchPhase::Decode) return;
    for (std::size_t index = 0; index < definition_.regions.size(); ++index) {
        const auto & region = definition_.regions[index];
        if (region.binding != "sglang.npu.allocator.decode") continue;
        std::optional<bool> branch;
        for (const auto node_index : region.nodes) {
            const auto & node = definition_.graph[node_index];
            if (node.kind != SourceSpanKind::Compute) continue;
            const auto layout = definition_.kv_kernel_layouts.find(definition_.observation(node).source_span);
            if (layout == definition_.kv_kernel_layouts.end() || !layout->second || !layout->second->allocation_branch()) return;
            const auto observed = *layout->second->allocation_branch();
            if (branch && *branch != observed) return;
            branch = observed;
        }
        // Region selection reuses the already qualified node branch; the core
        // never needs the native allocator class or the wrapper's binding name.
        if (branch) definition_.kv_region = KvRegionBranch{ KvRegionKind::Allocation, index, *branch };
    }
}

void SGLangTemplateBindings::bind_release_transfer(std::uint64_t sequence_tokens) {
    for (const auto & group : definition_.regions) {
        if (group.binding != "sglang.npu.allocator.free_group_end" || group.parent_call != definition_.source_call) continue;

        // TensorList shapes are absent in ATen. The nested free receives the
        // Cat result and supplies its actual one-dimensional INT32 descriptor.
        const SourceTensorLayout * indices = nullptr;
        for (const auto & child : definition_.regions) {
            if (child.parent_call != group.source_call || child.binding != "sglang.npu.allocator.free" || !child.module) continue;
            const auto & value = child.module->inputs.at("free_index");
            if (!value || indices) return;
            indices = &*value;
        }
        if (!indices || indices->dtype != "torch.int32" || indices->shape != std::vector<std::int64_t>{ static_cast<std::int64_t>(sequence_tokens) }
            || indices->stride != std::vector<std::int64_t>{ 1 })
            return;

        std::optional<TemplateNode> transfer;
        for (const auto & operation : definition_.operations) {
            if (operation.name != "aten::cat" || operation.device_spans.size() != 1) continue;
            const auto node = std::ranges::find(definition_.graph, operation.device_spans.front(), [this](const TemplateNode & candidate) {
                return definition_.observation(candidate).source_span;
            });
            if (node == definition_.graph.end() || definition_.observation(*node).source_scope != group.source_call) continue;
            if (transfer) return;
            transfer = *node;
        }
        if (!transfer || transfer->kind != SourceSpanKind::Dma || !definition_.observation(*transfer).transfer
            || definition_.observation(*transfer).transfer->direction != TransferDirection::DeviceToDevice
            || definition_.observation(*transfer).transfer->bytes != sequence_tokens * sizeof(std::int32_t))
            return;

        definition_.transfer_layouts.emplace(
            definition_.observation(*transfer).source_span,
            TransferParameterLayout{ TransferExtent::FinishedSequenceTokens, definition_.source_requests.size(), 1, sizeof(std::int32_t) });
    }
}

void SGLangTemplateBindings::bind_length_domain(std::uint32_t page_size_tokens, std::optional<std::uint32_t> sequence_capacity_tokens) {
    if (definition_.source_requests.empty() || !sequence_capacity_tokens) return;
    const auto count = definition_.source_requests.size();
    const bool prefill = definition_.phase == BatchPhase::Prefill;
    if (prefill) {
        for (std::size_t slot = 0; slot < count; ++slot) {
            const auto & request = definition_.source_requests[slot];
            if (!definition_.publication_finishes[slot] || request.context_tokens != 0 || request.input_tokens <= 1 || request.input_tokens > page_size_tokens)
                return;
        }
    }
    else {
        if (definition_.phase != BatchPhase::Decode) return;
        for (const auto & member : definition_.source_requests) {
            if (member.input_tokens != 1 || member.context_tokens == 0) return;
        }
    }

    // An observed input upload also needs its CPU work parameter before
    // changing lengths. Missing association cannot silently retain source work.
    for (const auto & region : definition_.regions) {
        if (region.binding != "sglang.inputs.resolve" || !region.module || region.module->result.at("input_copy").is_null()) continue;
        std::size_t bound = 0;
        for (const auto index : region.nodes) {
            const auto relation = definition_.transfer_layouts.find(definition_.observation(definition_.graph[index]).source_span);
            if (relation != definition_.transfer_layouts.end() && relation->second.extent == TransferExtent::InputTokens) ++bound;
        }
        if (bound != 1) return;
    }

    // Lengths describe each candidate member, independently of batch size.
    // Keep one cold page per Prefill member and one query per Decode member;
    // the complete member/operation rewrite still qualifies target structure.
    // Every device operation needs a parameter relation before admitting a
    // length candidate. Structural and control qualification still follows.
    for (const auto & node : definition_.graph) {
        const auto & node_observation = definition_.observation(node);
        if (node_observation.communication) continue;
        if (node_observation.transfer && !definition_.transfer_layouts.contains(node_observation.source_span)) return;
        if (node.kind == SourceSpanKind::Dma) {
            if (!node_observation.transfer || !definition_.transfer_layouts.contains(node_observation.source_span)) return;
            continue;
        }
        if (node.kind != SourceSpanKind::Compute) continue;
        if (!node_observation.kernel) return;
        if (definition_.model_kernel_layouts.contains(node_observation.source_span)
            || definition_.attention_kernel_layouts.contains(node_observation.source_span))
            continue;

        const auto batch = definition_.batch_kernel_layouts.find(node_observation.source_span);
        if (batch != definition_.batch_kernel_layouts.end() && batch->second) continue;
        const auto pool = definition_.request_pool_layouts.find(node_observation.source_span);
        if (pool != definition_.request_pool_layouts.end() && pool->second) continue;
        const auto kv = definition_.kv_kernel_layouts.find(node_observation.source_span);
        if (kv != definition_.kv_kernel_layouts.end() && kv->second) continue;
        return;
    }
    for (const auto & collective : definition_.collectives) {
        const auto & rule = definition_.graph.collective_rule(collective.envelope_node);
        if (!rule.count || !rule.work) return;
    }
    if (prefill) definition_.length_domain = RequestLengthDomain{ 2, page_size_tokens, 0, 0 };
    else {
        // Decode keeps one query per member. The native attention launch is
        // fixed; context changes its work and tiling, not the launch count.
        // Capacity admits a candidate; KV, page-table, CPU and publication
        // rewrites still have to qualify the complete invocation group.
        definition_.length_domain = RequestLengthDomain{ 1, 1, 1, *sequence_capacity_tokens - 1 };
    }
}

void SGLangTemplateBindings::bind_request_pool(const RequestPoolLayout & pool) {
    for (const auto & node : definition_.graph) {
        const auto & node_observation = definition_.observation(node);
        if (!node_observation.kernel) continue;
        auto layout = pool.bind(node_observation.source_span, *node_observation.kernel, definition_.source_requests);
        if (!layout) definition_.request_pool_layouts.emplace(node_observation.source_span, std::unexpected(layout.error()));
        else if (*layout) definition_.request_pool_layouts.emplace(node_observation.source_span, std::move(**layout));
    }

    for (const auto & operation : definition_.operations) {
        const auto * binding = pool.indexed_write(operation.host.source_span);
        if (!binding) continue;

        // Import the write branch, never the donor's pool contents. Every device
        // member must rebind its capacity, batch axes and indexed target slots.
        auto variant = binding->variant;
        variant.request_pool_rebindable = true;
        for (const auto span : operation.device_spans) {
            const auto layout = definition_.request_pool_layouts.find(span);
            if (layout == definition_.request_pool_layouts.end() || !layout->second) {
                variant.request_pool_rebindable = false;
                break;
            }
        }
        definition_.batch_operations.emplace(operation.host.source_span, variant);
    }
}

void SGLangTemplateBindings::bind_regions(const nlohmann::json & regions) {
    std::map<std::size_t, TemplateRegion *> local_regions;
    for (auto & region : definition_.regions) local_regions.emplace(region.source_call, &region);

    // Topology and local cuts are already assembled. Only the producer knows
    // how a native binding interprets the recorded module parameters.
    const SGLangBindings bindings;
    for (const auto & source : regions) {
        if (source.at("serving_call").get<std::size_t>() != definition_.source_call || source.at("module").is_null()) continue;
        auto & region = *local_regions.at(source.at("source_call").get<std::size_t>());
        region.module = bindings.region(region.binding.value()).read(source.at("module"));
        if (region.module->semantic == "decode_update_prefix") {
            const auto & parameters = region.module->call_parameters;
            const auto index = parameters.at("filter_prefix_count");
            const auto full = parameters.at("batch_is_full");
            if (definition_.step != StepKind::Schedule || definition_.phase == BatchPhase::Prefill || region.parent_call != definition_.source_call || index < 0
                || static_cast<std::size_t>(index) >= definition_.filters.size() || (full != 0 && full != 1)
                || parameters.at("request_count") != static_cast<std::int64_t>(definition_.filters[static_cast<std::size_t>(index)].input_count))
                throw std::invalid_argument("native decode update disagrees with its running filter");
        }
        if (region.module->semantic == "batch_transition") {
            const auto & parameters = region.module->call_parameters;
            const auto pending = parameters.at("pending_prefill_requests");
            const auto present = parameters.at("has_pending_prefill");
            const auto full = parameters.at("batch_is_full");
            if (definition_.step != StepKind::Schedule || region.parent_call != definition_.source_call || parameters.at("running_requests") < 0 || pending < 0
                || (present != 0 && present != 1) || (full != 0 && full != 1) || (present == 0 && pending != 0))
                throw std::invalid_argument("native batch transition has invalid Schedule entry state");
            if (present != 0 && (definition_.filters.empty() || definition_.filters.front().input_count != static_cast<std::size_t>(pending)))
                throw std::invalid_argument("native pending prefill disagrees with its first filter");

            definition_.transition = BatchTransitionState{ static_cast<std::size_t>(parameters.at("running_requests")),
                                                           present != 0 ? std::optional(static_cast<std::size_t>(pending)) : std::nullopt,
                                                           full != 0 };
        }
        if (region.module->semantic == "admission_prefix") {
            const auto & parameters = region.module->call_parameters;
            const auto waiting = parameters.at("waiting_requests");
            const auto running = parameters.at("running_requests");
            const auto full = parameters.at("batch_is_full");
            if (definition_.step != StepKind::Schedule || region.parent_call != definition_.source_call || waiting < 0 || running < 0
                || (full != 0 && full != 1))
                throw std::invalid_argument("native admission prefix has invalid Schedule entry state");

            // In this fixed non-chunked path, selected prefill members come from
            // the entry queue; a full running batch returns before admission.
            if (definition_.phase == BatchPhase::Prefill && (full != 0 || waiting < static_cast<std::int64_t>(definition_.source_requests.size())))
                throw std::invalid_argument("native admission entry state disagrees with selected prefill");

            definition_.admission = AdmissionEntryState{ static_cast<std::size_t>(waiting), static_cast<std::size_t>(running), full != 0 };
        }
        if (region.module->semantic == "last_locations") {
            const bool cold = std::ranges::all_of(definition_.source_requests, [](const auto & request) { return request.context_tokens == 0; });
            const auto & parameters = region.module->call_parameters;
            if (definition_.step != StepKind::Prepare || definition_.phase != BatchPhase::Prefill || region.parent_call != definition_.source_call
                || parameters.at("request_count") != static_cast<std::int64_t>(definition_.source_requests.size())
                || parameters.at("cold_prefix") != static_cast<std::int64_t>(cold))
                throw std::invalid_argument("native last-location construction disagrees with its Prepare batch");
        }
        if (region.module->request_id) {
            const auto request = std::ranges::find(definition_.source_requests, *region.module->request_id, &SourceRequest::id);
            if (request == definition_.source_requests.end()) throw std::invalid_argument("native request region is outside its serving batch");
            region.request_slot = static_cast<std::size_t>(request - definition_.source_requests.begin());
        }
        if (region.module->semantic == "request_publication") {
            const auto index = region.module->call_parameters.at("request_index");
            if (definition_.step != StepKind::Publish || region.parent_call != definition_.source_call || !region.request_slot || index < 0
                || static_cast<std::size_t>(index) != *region.request_slot)
                throw std::invalid_argument("native publication iteration disagrees with its request slot");
        }
    }

    // Nesting carries request ownership into allocator/prefix children. Resolve
    // every explicit identity first, so source record order cannot change it.
    for (auto & region : definition_.regions) {
        auto parent = local_regions.find(region.parent_call);
        if (region.module && region.module->semantic == "last_location") {
            const auto & parameters = region.module->call_parameters;
            if (parent == local_regions.end() || !parent->second->module || parent->second->module->semantic != "last_locations" || !region.request_slot
                || parameters.at("request_index") != static_cast<std::int64_t>(*region.request_slot)
                || parameters.at("cold_prefix") != static_cast<std::int64_t>(definition_.source_requests[*region.request_slot].context_tokens == 0))
                throw std::invalid_argument("native last-location member disagrees with its request identity");
        }
        while (parent != local_regions.end()) {
            if (parent->second->request_slot) {
                if (region.request_slot && region.request_slot != parent->second->request_slot)
                    throw std::invalid_argument("nested native regions disagree on request identity");
                region.request_slot = parent->second->request_slot;
                break;
            }
            parent = local_regions.find(parent->second->parent_call);
        }
    }

    // Prefix membership and decode-update membership have separate scopes.
    // The supported native prefix filters a pending prefill exactly once.
    if (definition_.step != StepKind::Schedule || !definition_.transition) return;
    const auto transition =
        std::ranges::find_if(definition_.regions, [](const auto & region) { return region.module && region.module->semantic == "batch_transition"; });
    if (transition == definition_.regions.end()) return;
    const auto prefix_count = definition_.transition->pending_prefill_requests ? 1U : 0U;
    std::size_t observed_prefix_filters = 0;
    std::size_t observed_update_filters = 0;
    const TemplateRegion * update_region = nullptr;
    std::size_t observed_merges = 0;
    // An empty decision can still execute update_running_batch. Derive its
    // presence from the observed region, not from the selected batch phase.
    const auto update_count =
        std::ranges::count_if(definition_.regions, [](const auto & region) { return region.module && region.module->semantic == "decode_update_prefix"; });
    if (update_count > 1 || (definition_.phase == BatchPhase::Prefill && update_count != 0) || (definition_.phase == BatchPhase::Decode && update_count != 1)
        || definition_.filters.size() != prefix_count + static_cast<std::size_t>(update_count))
        return;
    for (const auto & region : definition_.regions) {
        if (!region.module || (region.module->semantic != "batch_filter" && region.module->semantic != "batch_merge")) continue;
        auto parent = local_regions.find(region.parent_call);
        while (parent != local_regions.end() && parent->second != &*transition
               && (!parent->second->module || parent->second->module->semantic != "decode_update_prefix"))
            parent = local_regions.find(parent->second->parent_call);
        if (parent == local_regions.end()) return;
        const bool in_prefix = parent->second == &*transition;
        if (region.module->semantic == "batch_merge") {
            if (!in_prefix || !definition_.merge) return;
            ++observed_merges;
            continue;
        }
        const auto index = region.module->call_parameters.at("filter_index");
        if (in_prefix) {
            if (prefix_count != 1 || index != 0) return;
            ++observed_prefix_filters;
        }
        else {
            if (definition_.phase == BatchPhase::Prefill || index != prefix_count) return;
            ++observed_update_filters;
            update_region = parent->second;
        }
    }
    if (observed_prefix_filters != prefix_count || observed_update_filters != static_cast<std::size_t>(update_count)
        || observed_merges != static_cast<std::size_t>(definition_.merge.has_value()))
        return;
    definition_.transition_region = static_cast<std::size_t>(transition - definition_.regions.begin());
    // The ordinary prefix preserves or clears the latch; it never reads it
    // to select work. Admission consumes the resulting target state separately.
    definition_.transition_latch_parameterized = true;

    // Empty/all-kept filters return before creating an index tensor. The
    // surrounding non-merge branches only assign batch membership and latch;
    // the controller owns those effects, while this region retains CPU work.
    const bool scalar_filter = prefix_count == 0 || definition_.filters.front().retained_indices.empty()
                               || definition_.filters.front().retained_indices.size() == definition_.filters.front().input_count;
    definition_.transition_cpu_parameters = !definition_.merge && scalar_filter && definition_.is_cpu_region(*transition);

    // Native retract_decode always calls filter_batch again, even when it
    // retains its last request. One observed update filter and an unchanged
    // nonempty result therefore qualify the ordinary ratio-decay path.
    if (observed_update_filters != 1 || definition_.filters.back().retained_indices.size() != definition_.source_requests.size()) return;
    if (definition_.phase == BatchPhase::Decode && !definition_.source_requests.empty())
        definition_.running_update_region = static_cast<std::size_t>(update_region - definition_.regions.data());
    else if (definition_.phase == BatchPhase::Idle && definition_.source_requests.empty() && definition_.filters.back().input_count > 0
             && definition_.is_cpu_region(*update_region)) {
        // The empty return precedes memory checks, retraction and ratio decay.
        // Only CPU work is parameterized; retain its nodes and dependencies.
        definition_.running_update_region = static_cast<std::size_t>(update_region - definition_.regions.data());
    }
}

void SGLangTemplateBindings::bind_admission_work() {
    if (!definition_.admission) return;

    const TemplateRegion * admission = nullptr;
    const TemplateRegion * statistics = nullptr;
    for (const auto & region : definition_.regions) {
        if (!region.module) continue;
        if (region.module->semantic == "admission_prefix") admission = &region;
        if (region.module->semantic == "prefill_statistics") statistics = &region;
    }
    if (!admission || !admission->module->result.contains("admission_work")) return;

    const auto & entry = *definition_.admission;
    const auto & observed = admission->module->result.at("admission_work");
    AdmissionWork work;
    work.candidate_checks = observed.at("candidate_checks").get<std::size_t>();
    const auto stop = observed.at("native_stop").get<std::string>();
    if (stop == "queue_exhausted") work.stop = AdmissionStop::QueueExhausted;
    else if (stop == "request_capacity") work.stop = AdmissionStop::RequestCapacity;
    else if (stop == "early_return" && work.candidate_checks == 0 && entry.admission_blocked) work.stop = AdmissionStop::Closed;
    else if (stop == "early_return" && work.candidate_checks == 0 && entry.waiting_requests == 0) work.stop = AdmissionStop::NoWaitingRequests;
    else return; // NO_TOKEN/OTHER require qualification of the actual native exit path.

    // The supported path accepts every checked candidate. Native accepted-count
    // positions give member order independently of region serialization order.
    std::map<std::size_t, std::size_t> members;
    for (std::size_t index = 0; index < definition_.regions.size(); ++index) {
        const auto & region = definition_.regions[index];
        if (!region.module || region.module->semantic != "admission_budget" || region.parent_call != admission->source_call) continue;
        const auto & result = region.module->result;
        if (!result.contains("horizon_checks") || !result.at("candidate_accepted").get<bool>()) return;
        const auto slot = static_cast<std::size_t>(region.module->call_parameters.at("accepted_requests"));
        if (!members.emplace(slot, index).second) return;
    }
    if (members.size() != observed.at("request_checks").get<std::size_t>() || members.size() > work.candidate_checks) return;
    for (const auto & [slot, index] : members) {
        if (slot != work.request_horizon_checks.size()) return;
        work.request_horizon_checks.push_back(definition_.regions[index].module->result.at("horizon_checks").get<std::size_t>());
    }

    // Queue scans are separate native work. Qualify the derived common counts
    // against both the prefix observation and the post-Prepare region.
    const auto admitted = definition_.phase == BatchPhase::Prefill ? definition_.source_requests.size() : 0;
    if (observed.at("queue_compaction_items").get<std::size_t>() != (admitted == 0 ? 0 : entry.waiting_requests)) return;
    if (admitted != 0) {
        if (!statistics || statistics->parent_call != definition_.source_call) return;
        const auto & parameters = statistics->module->call_parameters;
        if (parameters.at("has_chunked_request") != 0 || parameters.at("waiting_requests") != static_cast<std::int64_t>(entry.waiting_requests - admitted)
            || parameters.at("running_requests") != static_cast<std::int64_t>(entry.running_requests))
            return;
    }
    else if (statistics) return;

    definition_.admission_work = work;
    if (!work.parameterized_cpu_path(entry, admitted)) return;

    // Parameterization retains every node, boundary and external dependency.
    // Replacement-port eligibility is separate: these regions are not removed.
    for (const auto * region : { admission, statistics }) {
        // Early returns have no Prepare or trailing statistics region.
        if (!region) continue;
        if (!definition_.is_cpu_region(*region)) return;
    }
    definition_.admission_cpu_request_count = admitted;

    // Shared prefix/queue/statistics work stays in the recipient. Only the
    // ordered successful budget calls belong to the replaceable member loop.
    std::vector<std::size_t> member_regions;
    std::vector<TemplateRegionReference> sequence;
    for (const auto & [slot, index] : members) {
        const auto & member = definition_.regions[index];
        if (member.module->result.at("native_result") != "CONTINUE" || !definition_.is_cpu_region(member)) return;
        member_regions.push_back(index);
        sequence.push_back({ &definition_, index });
    }
    if (member_regions.empty() || !definition_.plan_region_sequence(member_regions, sequence)) return;
    for (const auto & [slot, index] : members) definition_.regions[index].request_slot = slot;
    definition_.admission_regions = std::move(member_regions);
}

void SGLangTemplateBindings::bind_publication_output() {
    if (definition_.step != StepKind::Publish || !definition_.output_policy || definition_.source_requests.empty()) return;

    const TemplateRegion * output = nullptr;
    for (const auto & region : definition_.regions) {
        if (!region.module || region.module->semantic != "batch_output") continue;
        if (output) return;
        output = &region;
    }
    if (!output || output->parent_call != definition_.source_call || !definition_.is_cpu_region(*output) || output->inputs.size() != 1
        || output->outputs.size() != 1)
        return;

    const auto & module = *output->module;
    const auto observed = module.result.find("output_work");
    if (observed == module.result.end() || module.parameters.at("is_generation") != 1 || module.call_parameters.at("return_logprob") != 0) return;
    const auto & work = *observed;
    if (work.at("force_interval_tokens") != definition_.output_policy->interval_tokens || work.at("speculative").get<bool>()
        || work.at("disaggregation_mode") != "null" || work.at("request_time_stats").get<bool>())
        return;
    const auto & before_rows = work.at("before");
    const auto & after_rows = work.at("after");
    const auto count = definition_.source_requests.size();
    if (before_rows.size() != count || after_rows.size() != count) return;
    const auto context_limit = work.at("decode_context_limit").get<std::uint32_t>();
    for (std::size_t slot = 0; slot < count; ++slot) {
        const auto & request = definition_.source_requests[slot];
        const auto finished = definition_.publication_finishes[slot];
        const auto expected_tokens = static_cast<std::uint64_t>(request.generated_tokens) + 1;
        if (!finished || expected_tokens > definition_.output_policy->interval_tokens
            || (!*finished && expected_tokens == definition_.output_policy->interval_tokens))
            return;
        const auto & before = before_rows[slot];
        const auto & after = after_rows[slot];

        // First completed output has no prior detokenization or transmission. The
        // observed offsets prove the native branch; policy arithmetic alone cannot.
        for (const auto key :
             { "skipped", "stream", "return_hidden_states", "return_routed_experts", "return_indexer_topk", "customized_info", "multimodal" }) {
            if (before.at(key).get<bool>()) return;
        }
        // Native Req initializes this flag to None, then stores True after output.
        // Preserve that observed state instead of treating null as missing metadata.
        if (!before.at("finished_output").is_null() && before.at("finished_output") != false) return;
        if (before.at("request_id") != request.id || after.at("request_id") != request.id || before.at("finished").get<bool>() != *finished
            || before.at("generated_tokens") != expected_tokens || before.at("prompt_tokens") != request.prompt_tokens || before.at("send_token_offset") != 0
            || before.at("send_decode_id_offset") != 0 || !before.at("read_offset").is_null() || !before.at("surr_offset").is_null()
            || before.at("decoded_text_characters") != 0)
            return;
        if (!before.at("finished_len").is_null() && before.at("finished_len") != expected_tokens) return;

        if (!*finished) {
            if (before != after) return;
            continue;
        }
        const auto context = std::min(request.prompt_tokens, context_limit);
        if (!after.at("finished").get<bool>() || !after.at("finished_output").get<bool>() || after.at("generated_tokens") != expected_tokens
            || after.at("finished_len") != expected_tokens || after.at("send_token_offset") != expected_tokens
            || after.at("send_decode_id_offset") != expected_tokens + context || after.at("read_offset") != request.prompt_tokens
            || after.at("surr_offset") != request.prompt_tokens - context || after.at("decoded_text_characters") != 0)
            return;
    }

    // Observer exclusion can split the shared output body. One source anchor
    // identifies its semantics; complete regions preserve relocation and waits.
    std::optional<SourceSpanId> anchor;
    for (const auto index : output->nodes) {
        const auto & node = definition_.graph[index];
        if (node.kind != SourceSpanKind::CpuControl) continue;
        anchor = definition_.observation(node).source_span;
        break;
    }
    if (!anchor) return;
    definition_.publication_layout.emplace(*anchor, definition_.output_policy->interval_tokens, context_limit, work.at("sender_enabled").get<bool>());
    // Decode's member cuts are CPU-only; Prefill owns its prefix-copy or
    // direct-release work. Both keep shared output outside the member region.
    std::vector<std::optional<std::size_t>> members(count);
    for (std::size_t index = 0; index < definition_.regions.size(); ++index) {
        const auto & region = definition_.regions[index];
        if (!region.module || region.module->semantic != "request_publication") continue;
        if (region.parent_call != definition_.source_call || !region.request_slot) return;
        if (definition_.phase == BatchPhase::Decode) {
            if (!definition_.is_cpu_region(region) || region.inputs.size() != 1 || region.outputs.size() != 1) return;
        }
        else if (definition_.phase == BatchPhase::Prefill) {
            if (!definition_.region_ports(definition_.semantic_region(region))) return;
        }
        else return;

        auto & member = members[*region.request_slot];
        if (member) return;
        member = index;
    }
    if (std::ranges::any_of(members, [](const auto & member) { return !member; })) return;
    for (const auto & member : members) definition_.publication_regions.push_back(*member);
}

void SGLangTemplateBindings::bind_publication_progress() {
    if (definition_.step != StepKind::Publish || definition_.phase != BatchPhase::Decode || !definition_.output_policy || definition_.source_requests.empty())
        return;

    std::vector<bool> covered(definition_.source_requests.size(), false);
    for (std::size_t slot = 0; slot < covered.size(); ++slot) {
        if (definition_.publication_finishes[slot] != std::optional(false)
            || definition_.output_policy->evaluate(definition_.source_requests[slot].generated_tokens, false).emits)
            return;
    }
    for (const auto & region : definition_.regions) {
        if (!region.module || region.module->semantic != "request_publication") continue;
        const auto slot = *region.request_slot;
        if (covered[slot] || !definition_.is_cpu_region(region)) return;
        covered[slot] = true;
    }
    if (std::ranges::find(covered, false) != covered.end()) return;

    // No token payload is emitted, but the native streamer still sends an
    // empty batch message. Preserve its shared control work and completion
    // waits even though host CPU service time is outside the pricing scope.
    for (const auto & node : definition_.graph) {
        if (node.kind == SourceSpanKind::CpuObservation || node.kind == SourceSpanKind::CpuControl) continue;
        if (node.kind != SourceSpanKind::HostWait
            || std::ranges::find(definition_.stream_waits, definition_.observation(node).source_span, &TemplateStreamWait::source_span)
                   == definition_.stream_waits.end())
            return;
    }
    definition_.publication_cpu_progress = true;
}

void SGLangTemplateBindings::bind(const nlohmann::json & graph, const nlohmann::json & regions, const nlohmann::json & calls, const SGLangCopyLayout & copies,
                                  std::uint32_t page_size_tokens) {
    bind_regions(regions);
    bind_admission_work();
    bind_publication_progress();
    bind_publication_output();

    std::map<std::size_t, NativeFilterSelection> filter_scopes;
    std::unordered_set<std::size_t> merge_scopes;
    for (const auto & region : definition_.regions) {
        if (!region.module || (region.binding != "sglang.batch.merge" && region.binding != "sglang.batch.filter")) continue;

        const bool direct = region.parent_call == definition_.source_call;
        const auto parent = std::ranges::find(definition_.regions, region.parent_call, &TemplateRegion::source_call);
        const bool transition = parent != definition_.regions.end() && parent->module && parent->module->semantic == "batch_transition"
                                && parent->parent_call == definition_.source_call;
        const bool update = parent != definition_.regions.end() && parent->module && parent->module->semantic == "decode_update_prefix"
                            && parent->parent_call == definition_.source_call;
        if (region.binding == "sglang.batch.merge") {
            const auto & parameters = region.module->call_parameters;
            // Merge precedes selection of the next phase. A following prefill
            // has no running filter from which to infer the merged batch.
            const auto left = parameters.at("left_requests");
            const auto right = parameters.at("right_requests");
            if (definition_.step != StepKind::Schedule || (!direct && !transition) || !merge_scopes.empty() || parameters.at("filter_prefix_count") != 1
                || definition_.filters.empty() || left <= 0 || right <= 0
                || right != static_cast<std::int64_t>(definition_.filters.front().retained_indices.size()))
                throw std::invalid_argument("native merge region disagrees with the enclosing batch sequence");

            const BatchMerge observed{ static_cast<std::size_t>(left), static_cast<std::size_t>(right) };
            if (definition_.merge && *definition_.merge != observed)
                throw std::invalid_argument("native merge region disagrees with recorded filter membership");
            definition_.merge = observed;
            merge_scopes.insert(region.source_call);
            continue;
        }
        const auto index = region.module->call_parameters.at("filter_index");
        if (definition_.step != StepKind::Schedule || (!direct && !transition && !update) || (transition && index != 0)
            || (update && index < parent->module->call_parameters.at("filter_prefix_count")) || index < 0
            || static_cast<std::size_t>(index) >= definition_.filters.size())
            throw std::invalid_argument("native batch filter region has no enclosing Schedule selection");

        // Only a qualified parent identifies a movable filtering operation.
        // Retraction's additional filters are outside this supported update.
        if (transition || (update && index == parent->module->call_parameters.at("filter_prefix_count"))) {
            const auto role = transition ? BatchFilterBinding::Transition : BatchFilterBinding::RunningUpdate;
            filter_scopes.emplace(region.source_call, NativeFilterSelection{ static_cast<std::size_t>(index), role });

            // A known helper supplies parameter bindings; moving its region also
            // requires the enclosing transition or ordinary update qualification.
            const auto qualified_parent = transition ? definition_.transition_region : definition_.running_update_region;
            if (qualified_parent && *qualified_parent == static_cast<std::size_t>(parent - definition_.regions.begin()))
                definition_.filter_regions.emplace(role, static_cast<std::size_t>(&region - definition_.regions.data()));
        }
    }

    copies.bind(definition_, calls, filter_scopes);

    for (const auto & node : definition_.graph) {
        const auto & node_observation = definition_.observation(node);
        if (!node_observation.kernel) continue;
        const auto filter_scope = node_observation.source_scope ? filter_scopes.find(*node_observation.source_scope) : filter_scopes.end();
        const bool merge_scope = node_observation.source_scope && merge_scopes.contains(*node_observation.source_scope);
        if (definition_.step == StepKind::Schedule
            && (node_observation.source_scope == definition_.source_call || filter_scope != filter_scopes.end() || merge_scope)) {
            const auto selection = filter_scope == filter_scopes.end() ? std::nullopt : std::optional(filter_scope->second);
            const auto unscoped_merge = merge_scopes.empty() ? definition_.merge : std::nullopt;
            auto batch = merge_scope ? BatchLayout{}.bind_merge(*node_observation.kernel, definition_.merge)
                                     : BatchLayout{}.bind(*node_observation.kernel, definition_.filters, unscoped_merge, selection);
            if (!batch) definition_.batch_kernel_layouts.emplace(node_observation.source_span, std::unexpected(batch.error()));
            else if (*batch) definition_.batch_kernel_layouts.emplace(node_observation.source_span, std::move(**batch));
        }
        if (definition_.step == StepKind::Prepare && definition_.phase == BatchPhase::Decode && page_size_tokens > 1
            && node_observation.source_scope == definition_.source_call) {
            auto batch = BatchLayout{}.bind_decode_lengths(*node_observation.kernel, definition_.source_requests.size());
            if (!batch) definition_.batch_kernel_layouts.emplace(node_observation.source_span, std::unexpected(batch.error()));
            else if (*batch) definition_.batch_kernel_layouts.emplace(node_observation.source_span, std::move(**batch));
        }
    }

    bind_request_initialization();
    bind_last_locations(page_size_tokens);
    bind_decode_locations();
    bind_prefix_publication();
    bind_layouts(graph, calls, page_size_tokens);
    bind_rotary_positions();
    const auto counts = bind_collectives();
    for (const auto & collective : definition_.collectives) {
        const bool serial_completion = collective.completion && collective.entry_boundaries.size() == 1 && collective.exit_boundaries.size() == 1;
        const auto count = counts.find(collective.envelope_node);
        CollectiveParameterRule parameters{ count == counts.end() ? std::nullopt : std::optional(count->second),
                                            HcclMeshLayout{}.bind(collective.observation, serial_completion) };
        const auto [rule, inserted] = collective_rules_.insert(std::move(parameters));
        definition_.graph.bind_collective_rule(collective.envelope_node, *rule);
    }
    bind_logits_reshape();
    bind_transition_merge();
}

void SGLangTemplateBindings::bind_transition_merge() {
    if (!definition_.transition_region || !definition_.merge) return;
    const auto & prefix = definition_.filters.front();
    if (prefix.retained_indices.size() != prefix.input_count) return;

    // The all-kept pending batch follows the same nonempty merge branch.
    // Qualify the entire prefix, not just its Concat shapes: copies, waits,
    // communication or unbound device work require a separate structural rule.
    const auto cut = definition_.semantic_region(definition_.regions[*definition_.transition_region]);
    bool has_merge_kernel = false;
    for (const auto index : cut.nodes) {
        const auto & node = definition_.graph[index];
        if (node.kind == SourceSpanKind::CpuObservation || node.kind == SourceSpanKind::CpuControl) continue;
        if (node.kind != SourceSpanKind::Compute) return;

        const auto & observation = definition_.observation(node);
        if (!observation.kernel) return;
        const auto layout = definition_.batch_kernel_layouts.find(observation.source_span);
        if (layout == definition_.batch_kernel_layouts.end() || !layout->second) return;
        const auto merge = BatchLayout{}.bind_merge(*observation.kernel, definition_.merge);
        if (!merge || !*merge) return;
        has_merge_kernel = true;
    }
    definition_.transition_merge_parameters = has_merge_kernel;
}

void SGLangTemplateBindings::bind_prefix_publication() {
    if (definition_.step != StepKind::Publish || definition_.phase != BatchPhase::Prefill) return;

    // Resolve native request identity at production. Core parameters only see
    // ordered slots; changing membership still requires a qualified rewrite.
    std::map<std::size_t, std::size_t> scopes;
    for (const auto & region : definition_.regions) {
        if (region.binding != "sglang.chunk_cache.publish_prefix" || !region.module || !region.request_slot) continue;
        const auto slot = *region.request_slot;
        const auto & request = definition_.source_requests[slot];
        const auto length = static_cast<std::int64_t>(request.context_tokens) + request.input_tokens;
        if (region.module->call_parameters.at("sequence_tokens") != length)
            throw std::invalid_argument("native prefix publication length disagrees with its request");
        scopes.emplace(region.source_call, slot);
    }

    // An unscoped singleton has one possible owner. A batch requires explicit
    // request evidence; kernel order and equal shapes never identify a slot.
    if (scopes.empty() && definition_.source_requests.size() == 1) scopes.emplace(definition_.source_call, 0);
    for (const auto & [scope, slot] : scopes) {
        if (definition_.publication_finishes[slot] != std::optional(false)) continue;
        const auto & request = definition_.source_requests[slot];
        const auto length = static_cast<std::int64_t>(request.context_tokens) + request.input_tokens;
        std::optional<TemplateNode> selected;
        bool ambiguous = false;
        for (const auto & node : definition_.graph) {
            const auto & node_observation = definition_.observation(node);
            if (!node_observation.kernel || node_observation.source_scope != scope) continue;
            const auto & kernel = *node_observation.kernel;
            if (kernel.name != "aclnnInplaceCopy_CastAiCore_Cast" || kernel.operation != "Cast" || kernel.input_dtypes != "INT32"
                || kernel.output_dtypes != "INT64")
                continue;
            if (selected) {
                ambiguous = true;
                break;
            }
            selected = node;
        }
        if (!selected || ambiguous) continue;

        const KernelShapes inputs(definition_.observation(*selected).kernel->input_shapes);
        const KernelShapes outputs(definition_.observation(*selected).kernel->output_shapes);
        const std::vector<std::vector<std::int64_t>> expected{ { length } };
        if (inputs.tensors != expected || outputs.tensors != expected) continue;
        definition_.batch_kernel_layouts.emplace(definition_.observation(*selected).source_span,
                                                 BatchParameterLayout(std::nullopt,
                                                                      {
                                                                          { false, 0, BatchRowExtent::RequestSequenceTokens },
                                                                          {  true, 0, BatchRowExtent::RequestSequenceTokens }
        },
                                                                      BatchRequestBinding{ definition_.source_requests.size(), slot }));
    }
}

void SGLangTemplateBindings::bind_request_initialization() {
    if (definition_.step != StepKind::Prepare || definition_.phase != BatchPhase::Prefill || definition_.source_requests.empty()) return;

    // Each cold expression constructs the same scalar -1. Request identity is
    // supplied by its scope, never inferred from an operator name or position.
    std::vector<std::optional<std::size_t>> members(definition_.source_requests.size());
    std::optional<std::size_t> parent;
    for (std::size_t index = 0; index < definition_.regions.size(); ++index) {
        const auto & region = definition_.regions[index];
        if (!region.module || region.module->semantic != "last_location") continue;
        if (region.module->call_parameters.at("cold_prefix") != 1) return;
        // CPU-only ports can still enclose a native host wait. Its dependency
        // belongs to the sequence planner, not to service-only qualification.
        const auto ports = definition_.region_ports(definition_.semantic_region(region));
        if (!ports || ports->size() != 1 || ports->front().resource.domain != SourceDomain::CpuThread) return;

        if (parent && *parent != region.parent_call) return;
        parent = region.parent_call;
        auto & member = members[*region.request_slot];
        if (member) return;
        member = index;
    }
    if (std::ranges::any_of(members, [](const auto & member) { return !member; })) return;
    for (const auto member : members) definition_.request_initialization_regions.push_back(*member);
}

void SGLangTemplateBindings::bind_last_locations(std::uint32_t page_size_tokens) {
    if (definition_.step != StepKind::Prepare || definition_.phase != BatchPhase::Prefill || page_size_tokens <= 1 || definition_.source_requests.empty())
        return;
    for (const auto & request : definition_.source_requests) {
        if (request.context_tokens != 0) return;
    }

    // alloc_for_extend concatenates one INT64 last location per cold request.
    // Only the unique direct Cat belongs to this native expression; nested
    // allocator concatenations are different work and cannot be donors.
    const TemplateOperation * selected = nullptr;
    std::optional<TemplateNode> work;
    for (const auto & operation : definition_.operations) {
        if (operation.name != "aten::cat" || operation.device_spans.size() != 1) continue;
        const auto found = std::ranges::find(definition_.graph, operation.device_spans.front(), [this](const TemplateNode & candidate) {
            return definition_.observation(candidate).source_span;
        });
        if (found == definition_.graph.end() || definition_.observation(*found).source_scope != definition_.source_call) continue;
        if (selected) return;
        selected = &operation;
        work = *found;
    }
    if (!selected) return;

    // ACLNN groups this contiguous INT64 scalar-list path into at most 32
    // operands per ConcatD. Larger lists require a multi-operation template.
    const auto requests = definition_.source_requests.size();
    if (requests > last_location_concat_limit) return;
    if (requests == 1) {
        // Singleton Cat still copies one INT64 value. Its bytes do not depend
        // on prompt length, page count or the numeric last-location value.
        if (work->kind != SourceSpanKind::Dma || !definition_.observation(*work).transfer
            || definition_.observation(*work).transfer->direction != TransferDirection::DeviceToDevice
            || definition_.observation(*work).transfer->bytes != sizeof(std::int64_t))
            return;
        definition_.transfer_layouts.emplace(definition_.observation(*work).source_span,
                                             TransferParameterLayout{ TransferExtent::FixedAtRequestCount, requests });
    }
    else {
        if (!definition_.observation(*work).kernel) return;
        const auto & kernel = *definition_.observation(*work).kernel;
        const KernelShapes inputs(kernel.input_shapes);
        const KernelShapes outputs(kernel.output_shapes);
        std::string input_types = "INT64";
        std::string input_formats = "ND";
        for (std::size_t index = 1; index < requests; ++index) {
            input_types += ";INT64";
            input_formats += ";ND";
        }
        if (kernel.name != "aclnnCat_ConcatD_ConcatD" || kernel.operation != "ConcatD" || kernel.input_dtypes != input_types || kernel.output_dtypes != "INT64"
            || kernel.input_formats != input_formats || kernel.output_formats != "ND"
            || inputs.tensors != std::vector<std::vector<std::int64_t>>(requests, { 1 })
            || outputs.tensors != std::vector<std::vector<std::int64_t>>{ { static_cast<std::int64_t>(requests) } })
            return;
        definition_.batch_kernel_layouts.emplace(definition_.observation(*work).source_span,
                                                 BatchParameterLayout(std::nullopt,
                                                                      {
                                                                          { true, 0, BatchRowExtent::Requests }
        },
                                                                      std::nullopt,
                                                                      BatchInputArity::Requests));
    }

    // Singleton Cat is a copy; the multi-input branch remains one ConcatD
    // only inside the native grouping limit. Other batch work qualifies separately.
    definition_.batch_operations.emplace(selected->host.source_span,
                                         BatchOperationVariant{ BatchOperationRole::LastLocationConcat,
                                                                requests,
                                                                requests == 1 ? 1U : 2U,
                                                                requests == 1 ? 1U : last_location_concat_limit,
                                                                std::nullopt,
                                                                0,
                                                                true });
}

void SGLangTemplateBindings::bind_decode_locations() {
    if (definition_.step != StepKind::Prepare || definition_.phase != BatchPhase::Decode) return;

    // alloc_for_decode clones seq_lens before writing the request table.
    // Numeric positions change with context; the INT64 vector still has one
    // element per request. Nested allocator copies have different semantics.
    const TemplateOperation * selected = nullptr;
    std::optional<TemplateNode> work;
    for (const auto & operation : definition_.operations) {
        if (operation.name != "aten::clone" || operation.device_spans.size() != 1) continue;
        const auto found = std::ranges::find(definition_.graph, operation.device_spans.front(), [this](const TemplateNode & candidate) {
            return definition_.observation(candidate).source_span;
        });
        if (found == definition_.graph.end() || definition_.observation(*found).source_scope != definition_.source_call) continue;
        if (selected) return;
        selected = &operation;
        work = *found;
    }
    if (!selected || !selected->input_shapes || !selected->input_types) return;

    const auto requests = definition_.source_requests.size();
    const KernelShapes input(selected->input_shapes->substr(0, selected->input_shapes->find(';')));
    if (selected->input_types->substr(0, selected->input_types->find(';')) != "long int"
        || input.tensors != std::vector<std::vector<std::int64_t>>{ { static_cast<std::int64_t>(requests) } } || work->kind != SourceSpanKind::Dma
        || !definition_.observation(*work).transfer || definition_.observation(*work).transfer->direction != TransferDirection::DeviceToDevice
        || definition_.observation(*work).transfer->bytes != requests * sizeof(std::int64_t))
        return;

    definition_.transfer_layouts.emplace(definition_.observation(*work).source_span,
                                         TransferParameterLayout{ TransferExtent::RequestElements, requests, 0, sizeof(std::int64_t) });
}

void SGLangTemplateBindings::bind_layouts(const nlohmann::json & graph, const nlohmann::json & calls, std::uint32_t page_size_tokens) {
    std::map<SourceSpanId, ModelKernelLayout> model_layouts;

    // Use the recorded operation membership, not temporal proximity, to
    // identify both dtype groups emitted by the eager input registry.
    std::unordered_set<SourceSpanId> foreach_copy_spans;
    for (const auto & operation : definition_.operations) {
        if (operation.name == "aten::_foreach_copy_") foreach_copy_spans.insert(operation.device_spans.begin(), operation.device_spans.end());
    }

    std::int64_t serving_tokens = 0;
    for (const auto & request : definition_.source_requests) serving_tokens += request.input_tokens;

    std::map<std::size_t, const SourceNativeRegion *> modules;
    std::map<std::pair<std::size_t, std::string_view>, std::vector<const SourceNativeRegion *>> parameter_children;
    for (const auto & region : definition_.regions) {
        if (region.module) {
            modules.emplace(region.source_call, &*region.module);
            if (region.module->semantic == "logits_selection" || region.module->semantic == "embedding")
                parameter_children[{ region.parent_call, region.module->semantic }].push_back(&*region.module);
        }
    }

    for (const auto & node : definition_.graph) {
        const auto & node_observation = definition_.observation(node);
        if (!node_observation.kernel) continue;
        const auto & scope = graph.at("spans").at(node_observation.source_span).at("scope_call");
        if (scope.is_null()) continue;

        // Forward-input work belongs to its explicit native scope, not to a
        // model child with a coincidentally matching tensor shape.
        if (calls.at(scope.get<std::size_t>()).at("native_step") == "model_forward") {
            auto layout = ModelKernelLayout::bind_page_table(scope.get<std::size_t>(), *node_observation.kernel, definition_.source_requests, page_size_tokens);
            if (!layout)
                layout = ModelKernelLayout::bind_forward_inputs(scope.get<std::size_t>(),
                                                                *node_observation.kernel,
                                                                serving_tokens,
                                                                definition_.source_requests.size(),
                                                                definition_.step,
                                                                foreach_copy_spans.contains(node_observation.source_span));
            if (layout) model_layouts.emplace(node_observation.source_span, std::move(*layout));
            continue;
        }
        const auto module = modules.find(scope.get<std::size_t>());
        if (module == modules.end()) continue;
        if (module->second->semantic == "attention_metadata") {
            auto layout = ModelKernelLayout::bind_page_table(scope.get<std::size_t>(), *node_observation.kernel, definition_.source_requests, page_size_tokens);
            if (!layout)
                layout =
                    ModelKernelLayout::bind_sequence_lengths(*module->second, scope.get<std::size_t>(), *node_observation.kernel, definition_.source_requests);
            if (layout) model_layouts.emplace(node_observation.source_span, std::move(*layout));
            continue;
        }

        // Bind against the innermost native scope; a parent region must not
        // relabel a child's kernel merely because its dimensions coincide.
        const SourceNativeRegion * parameter_child = nullptr;
        const auto child_semantic = module->second->semantic == "model_body" ? "embedding" : "logits_selection";
        const auto children = parameter_children.find({ module->first, child_semantic });
        if (children != parameter_children.end() && children->second.size() == 1) parameter_child = children->second.front();
        auto layout = ModelKernelLayout::bind(*module->second,
                                              module->first,
                                              *node_observation.kernel,
                                              serving_tokens,
                                              definition_.source_requests.size(),
                                              definition_.step,
                                              parameter_child);
        if (layout) model_layouts.emplace(node_observation.source_span, std::move(*layout));
        auto attention = AttentionKernelLayout::bind(*module->second,
                                                     module->first,
                                                     *node_observation.kernel,
                                                     definition_.step,
                                                     definition_.source_requests,
                                                     page_size_tokens);
        if (attention) {
            const auto [rule, inserted] = attention_rules_.insert(attention->compile());
            definition_.attention_kernel_layouts.emplace(node_observation.source_span, AttentionParameterObservation{ &*rule, attention->describe() });
        }
    }

    // Bind the entire native divide call, including its materialization, casts
    // and worker. A shape-only edit cannot remove a now-unnecessary copy.
    // This call consumes the selected batch rows; backing pool capacity does
    // not change its operands or state. The materialization variant still must match.
    for (const auto & operation : definition_.operations) {
        if (operation.name != "aten::floor_divide") continue;
        std::multiset<ModelKernelRole> roles;
        for (const auto span : operation.device_spans) {
            const auto layout = model_layouts.find(span);
            if (layout != model_layouts.end()) roles.insert(layout->second.role());
        }
        if (roles.size() != operation.device_spans.size()) continue;

        auto materialization = PageTableMaterialization::Contiguous;
        if (roles.erase(ModelKernelRole::PageTableView)) materialization = PageTableMaterialization::Strided;
        else if (roles.erase(ModelKernelRole::PageTableSlice)) materialization = PageTableMaterialization::SingleColumn;
        if (roles != std::multiset<ModelKernelRole>{ ModelKernelRole::PageTableCast, ModelKernelRole::PageTableDivide, ModelKernelRole::PageTableCast })
            continue;
        if (operation.device_spans.size() != (materialization == PageTableMaterialization::Contiguous ? 3 : 4)) continue;

        definition_.batch_operations.emplace(operation.host.source_span,
                                             BatchOperationVariant{ BatchOperationRole::PageTableDivide,
                                                                    definition_.source_requests.size(),
                                                                    1,
                                                                    std::numeric_limits<std::uint64_t>::max(),
                                                                    materialization,
                                                                    page_size_tokens,
                                                                    true });
    }

    // Sampling belongs to model_forward but consumes its logits child's rows.
    // Require a unique qualified output; never infer B from matching numbers.
    std::map<std::size_t, std::vector<const ModelKernelLayout *>> logits_outputs;
    for (const auto & [span, layout] : model_layouts) {
        if (layout.role() != ModelKernelRole::LogitsCast) continue;
        const auto scope = graph.at("spans").at(span).at("scope_call").get<std::size_t>();
        logits_outputs[calls.at(scope).at("parent").get<std::size_t>()].push_back(&layout);
    }

    // More than one subtraction in the sampling scope is ambiguous without
    // finer provenance. Do not select by source order or matching dimensions.
    std::map<std::size_t, std::size_t> sampling_subtractions;
    for (const auto & node : definition_.graph)
        if (definition_.observation(node).kernel && definition_.observation(node).kernel->operation == "Sub" && definition_.observation(node).source_scope)
            ++sampling_subtractions[*definition_.observation(node).source_scope];

    for (const auto & node : definition_.graph) {
        const auto & node_observation = definition_.observation(node);
        if (!node_observation.kernel || model_layouts.contains(node_observation.source_span)) continue;
        const auto & scope = graph.at("spans").at(node_observation.source_span).at("scope_call");
        if (scope.is_null()) continue;
        auto forward_scope = scope.get<std::size_t>();
        const auto region = modules.find(forward_scope);
        if (region != modules.end() && region->second->semantic == "sampling") {
            if (region->second->call_parameters.at("is_all_greedy") != 1 || region->second->call_parameters.at("return_logprob") != 0) continue;
            forward_scope = calls.at(forward_scope).at("parent").get<std::size_t>();
        }
        if (calls.at(forward_scope).at("native_step") != "model_forward") continue;
        const auto outputs = logits_outputs.find(forward_scope);
        if (outputs == logits_outputs.end() || outputs->second.size() != 1) continue;
        auto layout = outputs->second.front()->bind_sampling(scope.get<std::size_t>(), *node_observation.kernel);
        if (!layout && region != modules.end() && region->second->semantic == "sampling" && sampling_subtractions[scope.get<std::size_t>()] == 1)
            layout = ModelKernelLayout::bind_sampling_positions(*region->second,
                                                                scope.get<std::size_t>(),
                                                                *node_observation.kernel,
                                                                definition_.source_requests,
                                                                definition_.step);
        if (layout) model_layouts.emplace(node_observation.source_span, std::move(*layout));
    }

    // Native binders are temporary production state. Keep only the compiled
    // parameter contract in the immutable template consumed by the core.
    for (const auto & node : definition_.graph) {
        const auto & node_observation = definition_.observation(node);
        const auto layout = model_layouts.find(node_observation.source_span);
        if (layout == model_layouts.end()) continue;

        const auto [rule, inserted] = model_rules_.insert(layout->second.compile(*node_observation.kernel));
        definition_.graph.bind_model_rule(node.observation_slot, *rule);
        definition_.model_kernel_layouts.emplace(node_observation.source_span, ModelParameterObservation{ node.observation_slot, layout->second.describe() });
    }
}

void SGLangTemplateBindings::bind_rotary_positions() {
    if (definition_.step != StepKind::Decode) return;
    std::map<std::size_t, const SourceNativeRegion *> modules;
    for (const auto & region : definition_.regions) {
        if (region.module && region.module->semantic == "qkv_preparation" && region.module->method == "forward_prepare_npu")
            modules.emplace(region.source_call, &*region.module);
    }

    std::vector<std::size_t> gathers;
    std::vector<std::size_t> tiles;
    std::vector<std::size_t> slices;
    std::vector<std::size_t> consumers;
    for (std::size_t index = 0; index < definition_.graph.size(); ++index) {
        const auto found = definition_.model_kernel_layouts.find(definition_.observation(definition_.graph[index]).source_span);
        if (found == definition_.model_kernel_layouts.end()) continue;
        switch (definition_.graph.model_rule(found->second.node).role()) {
        case ModelKernelRole::PositionGather:
            gathers.push_back(index);
            break;
        case ModelKernelRole::PositionTile:
            tiles.push_back(index);
            break;
        case ModelKernelRole::PositionSlice:
            slices.push_back(index);
            break;
        case ModelKernelRole::FusedQkv:
            consumers.push_back(index);
            break;
        default:
            break;
        }
    }
    if (gathers.size() != 1 || tiles.size() != 1 || consumers.empty()) return;
    const auto producer_scope = definition_.observation(definition_.graph[tiles.front()]).source_scope;
    const auto producer = modules.find(*producer_scope);
    if (producer == modules.end() || definition_.observation(definition_.graph[gathers.front()]).source_scope != producer_scope) return;

    std::uint64_t tokens = 0;
    for (const auto & request : definition_.source_requests) tokens += request.input_tokens;
    if ((tokens == 1 && !slices.empty()) || (tokens > 1 && slices.size() != 2)) return;
    for (const auto index : slices) {
        if (definition_.observation(definition_.graph[index]).source_scope != producer_scope) return;
    }
    for (const auto index : consumers) {
        const auto consumer = modules.find(*definition_.observation(definition_.graph[index]).source_scope);
        if (consumer == modules.end() || consumer->second->module_type != producer->second->module_type
            || consumer->second->parameters.at("head_dim") != producer->second->parameters.at("head_dim")
            || consumer->second->parameters.at("layer_index") < producer->second->parameters.at("layer_index"))
            return;
    }

    // Qwen3's first local layer refreshes the shared RotaryEmbedding once per
    // decode forward. Both views of Tile are ready at T=1; larger T materializes
    // them separately. We do not guess which observed Slice is sin versus cos.
    const auto & producers = tokens == 1 ? tiles : slices;
    for (const auto consumer : consumers) {
        for (const auto producer_node : producers) definition_.rotary_position_dependencies.push_back({ producer_node, consumer });
    }
}

void SGLangTemplateBindings::bind_logits_reshape() {
    definition_.logits_reshape_operation = logits_gather_reshape();
    if (!definition_.logits_reshape_operation) return;

    const auto & operation = definition_.operations[*definition_.logits_reshape_operation];
    const auto requests = definition_.source_requests.size();
    if (requests == 1) {
        if (!definition_.logits_view_service()) return;
    }
    else {
        if (operation.device_spans.size() != 1) return;
        const auto layout = definition_.model_kernel_layouts.find(operation.device_spans.front());
        if (layout == definition_.model_kernel_layouts.end() || definition_.graph.model_rule(layout->second.node).role() != ModelKernelRole::LogitsTranspose)
            return;
    }

    // The fixed TP>1 permuted [B, TP, Vrank] view is contiguous only at B=1.
    // Its operands depend on the active batch, not backing request-pool capacity.
    // This selects the native branch; operation ports still qualify its rewrite.
    definition_.batch_operations.emplace(operation.host.source_span,
                                         BatchOperationVariant{ BatchOperationRole::LogitsReshape,
                                                                requests,
                                                                requests == 1 ? 1U : 2U,
                                                                requests == 1 ? 1U : std::numeric_limits<std::uint64_t>::max(),
                                                                std::nullopt,
                                                                0,
                                                                true });
}

std::optional<std::size_t> SGLangTemplateBindings::logits_gather_reshape() const {
    std::optional<TemplateNode> projection;
    std::optional<TemplateNode> cast;
    std::map<SourceSpanId, TemplateNode> observed;
    for (const auto & node : definition_.graph) {
        const auto & node_observation = definition_.observation(node);
        observed.emplace(node_observation.source_span, node);
        const auto layout = definition_.model_kernel_layouts.find(node_observation.source_span);
        if (layout == definition_.model_kernel_layouts.end()) continue;
        if (definition_.graph.model_rule(layout->second.node).role() == ModelKernelRole::LogitsProjection) {
            if (projection) return std::nullopt;
            projection = node;
        }
        if (definition_.graph.model_rule(layout->second.node).role() == ModelKernelRole::LogitsCast) {
            if (cast) return std::nullopt;
            cast = node;
        }
    }
    if (!projection || !cast || !definition_.observation(*projection).source_scope
        || definition_.observation(*projection).source_scope != definition_.observation(*cast).source_scope)
        return std::nullopt;

    const KernelShapes projected(definition_.observation(*projection).kernel->output_shapes);
    const KernelShapes gathered(definition_.observation(*cast).kernel->input_shapes);
    if (projected.tensors.size() != 1 || gathered.tensors.size() != 1 || projected.tensors[0].size() != 2 || gathered.tensors[0].size() != 2)
        return std::nullopt;
    const auto width = projected.tensors[0][1];
    const auto total = gathered.tensors[0][1];
    if (width <= 0 || total <= width || total % width != 0) return std::nullopt;
    const std::vector<std::int64_t> shape{ static_cast<std::int64_t>(definition_.source_requests.size()), total / width, width };

    // The input is already permuted to [B,rank,Vrank]. This distinguishes the
    // final flattening from the earlier 2-D reshape, even when B equals rank.
    std::optional<std::size_t> selected;
    for (std::size_t index = 0; index < definition_.operations.size(); ++index) {
        const auto & operation = definition_.operations[index];
        if (operation.name != "aten::reshape" || !operation.input_shapes || !operation.input_types
            || definition_.observation(observed.at(operation.host.source_span)).source_scope != definition_.observation(*projection).source_scope)
            continue;
        const auto type = operation.input_types->substr(0, operation.input_types->find(';'));
        if (type != "c10::BFloat16" || definition_.observation(*projection).kernel->output_dtypes != "DT_BF16"
            || definition_.observation(*cast).kernel->input_dtypes != "DT_BF16")
            continue;
        const KernelShapes input(operation.input_shapes->substr(0, operation.input_shapes->find(';')));
        if (input.tensors.front() != shape) continue;
        if (selected) return std::nullopt;
        selected = index;
    }
    return selected;
}

std::map<std::size_t, CollectiveCountRule> SGLangTemplateBindings::bind_collectives() const {
    std::map<std::size_t, CollectiveCountRule> result;
    std::map<std::size_t, const SourceNativeRegion *> modules;
    for (const auto & region : definition_.regions)
        if (region.module) modules.emplace(region.source_call, &*region.module);

    std::int64_t tokens = 0;
    for (const auto & request : definition_.source_requests) tokens += request.input_tokens;
    for (const auto & collective : definition_.collectives) {
        const auto & observed = collective.observation;
        const auto scope = definition_.observation(definition_.graph[collective.envelope_node]).source_scope;
        if (!scope || observed.data_type != "BFP16") continue;
        const auto module = modules.find(*scope);
        if (module == modules.end()) continue;
        const auto & region = *module->second;

        CollectiveRows rows = CollectiveRows::Tokens;
        std::int64_t width = 0;
        if (observed.name.starts_with("hcom_allReduce__")) {
            if (region.semantic == "embedding") {
                const auto input = region.inputs.find("input_");
                const auto hidden = region.parameters.find("hidden_size");
                if (input == region.inputs.end() || !input->second || input->second->shape != std::vector<std::int64_t>{ tokens }
                    || hidden == region.parameters.end())
                    continue;
                width = hidden->second;
            }
            else if (region.semantic == "decoder_layer" || region.semantic == "feed_forward") {
                const auto input = region.inputs.find(region.semantic == "decoder_layer" ? "hidden_states" : "x");
                if (input == region.inputs.end() || !input->second || input->second->shape.size() != 2 || input->second->shape[0] != tokens
                    || input->second->dtype != "torch.bfloat16")
                    continue;
                width = input->second->shape[1];
            }
        }
        else if (observed.name.starts_with("hcom_allGather__") && region.semantic == "logits") {
            rows = CollectiveRows::Requests;
            std::optional<TemplateNode> projection;
            bool ambiguous = false;
            for (const auto & node : definition_.graph) {
                const auto & node_observation = definition_.observation(node);
                if (node_observation.source_scope != scope) continue;
                const auto layout = definition_.model_kernel_layouts.find(node_observation.source_span);
                if (layout == definition_.model_kernel_layouts.end()
                    || definition_.graph.model_rule(layout->second.node).role() != ModelKernelRole::LogitsProjection)
                    continue;
                if (projection) ambiguous = true;
                projection = node;
            }
            // Vocabulary partition width comes from the qualified projection,
            // never by dividing the observed collective count by batch size.
            if (!projection || ambiguous || definition_.observation(*projection).kernel->output_dtypes != "DT_BF16") continue;
            const KernelShapes output(definition_.observation(*projection).kernel->output_shapes);
            if (output.tensors.size() != 1 || output.tensors[0].size() != 2
                || output.tensors[0][0] != static_cast<std::int64_t>(definition_.source_requests.size()))
                continue;
            width = output.tensors[0][1];
        }

        const auto extent = rows == CollectiveRows::Tokens ? tokens : static_cast<std::int64_t>(definition_.source_requests.size());
        if (width <= 0 || extent <= 0 || observed.count != static_cast<std::uint64_t>(extent) * static_cast<std::uint64_t>(width)) continue;
        result.emplace(collective.envelope_node, CollectiveCountRule{ rows, static_cast<std::uint64_t>(width) });
    }
    return result;
}

} // namespace markov
