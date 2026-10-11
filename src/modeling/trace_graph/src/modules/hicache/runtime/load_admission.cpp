#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/cost/host_operation.hpp"
#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {


void HiCacheWriteCalls::prepare_generated_load_admissions(const patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues) {
    const auto & graph = source.graph();
    for (const auto & branch : load_branches_) {
        const auto key = std::pair{ branch.envelope.pid, branch.envelope.tid };
        if (std::ranges::any_of(load_admissions_, [&](const auto & sample) { return sample.pid == key.first && sample.tid == key.second; })) continue;
        std::optional<size_t> compute;
        const auto witness = [&](size_t node) {
            if (compute && graph.node(*compute).lane_id != graph.node(node).lane_id)
                throw std::runtime_error("Scheduler load admission has conflicting compute roles");
            compute = node;
        };
        if (const auto found = compute_resources_.find(key); found != compute_resources_.end()) witness(found->second);
        for (const auto & [id, donor] : templates_)
            if (donor.pid == key.first && donor.tid == key.second) witness(donor.compute_record);
        for (const auto & donor : eviction_controls_)
            if (donor.pid == key.first && donor.tid == key.second
                && (donor.region.kind == HiCacheEvictionRegion::Kind::ReleaseRegular || donor.region.kind == HiCacheEvictionRegion::Kind::ReleaseBackup)
                && donor.expansion.streams.size() == 1)
                witness(donor.expansion.streams.front().source_node);
        if (!compute) continue; // Required only if this target needs a new branch.
        const auto resources = observe_load_index_resources(source, queues, branch.entry_node, *compute);
        if (!resources) continue;
        const auto operation = [&](const char * name) {
            return LoadStep{
                generate_async_device_operation(graph, branch.entry_node, resources->worker, resources->compute, std::string("load index ") + name),
                name,
                { graph.node(*compute).lane_id }
            };
        };
        LoadAdmissionProgram program{ .pid = key.first, .tid = key.second, .main_lane = std::string(graph.node_lane_key(branch.entry_node)) };
        for (const auto * name : { "multiply", "range", "add" }) program.allocation.push_back(operation(name));
        program.clone = operation("clone");
        program.merge_concat = operation("merge_concat");
        program.merge_sort = operation("merge_sort");
        program.prefix_concat = operation("prefix_concat");
        load_admissions_.push_back(std::move(program));
    }
}

size_t HiCacheWriteCalls::select_load_admission(const std::string & pid, const std::string & tid) const {
    for (size_t i = 0; i < load_admissions_.size(); ++i)
        if (load_admissions_[i].pid == pid && load_admissions_[i].tid == tid) return i;

    throw std::runtime_error("Generated allocation lacks proven scheduler compute/worker resources");
}

void HiCacheWriteCalls::bind_load_resources(const patch::HiCacheSourceDagIndex & source, Call & call, const LoadAdmissionProgram & program) {
    if (program.clone) bind_call_resources(source, call, program.clone->program.plan, true);
    for (const auto & step : program.allocation) bind_call_resources(source, call, step.program.plan, true);
    if (allocator_need_sort_) {
        bind_call_resources(source, call, program.merge_concat.program.plan, true);
        bind_call_resources(source, call, program.merge_sort.program.plan, true);
    }
    bind_call_resources(source, call, program.prefix_concat.program.plan, true);
}

HiCacheHostExpansion HiCacheWriteCalls::cost_load_step(const LoadStep & step, uint64_t pages, bool first_clone) const {
    structure::OperationWork work{
        "hicache_io_control",
        "load.index." + step.operation,
        "ascend.paged_unsorted",
        "",
        { { "pages", static_cast<double>(pages) },
          { "page_size_tokens", static_cast<double>(page_size_) },
          { "requested_tokens", static_cast<double>(core::checked_multiply_u64(pages, page_size_, "Load token count overflow")) } }
    };
    if (step.operation == "clone") work.features["first_clone"] = first_clone ? 1.0 : 0.0;
    return bind_load_operation(step, std::move(work));
}

HiCacheHostExpansion HiCacheWriteCalls::bind_load_operation(const LoadStep & step, structure::OperationWork work) const {
    auto plan = step.program.plan;
    const auto costs = cost::estimate_host_operation(operator_provider_, std::move(work), true);
    bind_host_operation(plan, { step.program.submit, step.program.worker, step.program.device, step.program.worker_return }, costs);
    return plan;
}

HiCacheHostExpansion HiCacheWriteCalls::load_prefix_concat(const LoadAdmissionProgram & program, const model::HiCacheLoadAdmissionWork & work) const {
    const auto prefix_tokens = core::checked_multiply_u64(work.existing_prefix_pages, page_size_, "Prefix token count overflow");
    const auto new_tokens = work.allocated ? core::checked_multiply_u64(work.requested_pages, page_size_, "Load token count overflow") : 0;
    const auto output_tokens = core::checked_add_u64(prefix_tokens, new_tokens, "Concatenated prefix overflow");
    structure::OperationWork operation{
        "hicache_io_control",
        "load.admission.prefix_concat",
        "ascend.aclnn_cat",
        "cpu_service",
        { { "prefix_tokens", static_cast<double>(prefix_tokens) },
          { "new_tokens", static_cast<double>(new_tokens) },
          { "output_tokens", static_cast<double>(output_tokens) },
          { "nonempty_inputs", static_cast<double>((prefix_tokens > 0) + (new_tokens > 0)) } }
    };
    if (output_tokens) {
        auto plan = bind_load_operation(program.prefix_concat, operation);
        operation.operation = "load.admission.prefix_tail";
        const auto tail = cost::estimate_host_operation(operator_provider_, operation, false);
        bind_host_operation(plan, { .main = plan.host_return }, tail);
        return plan;
    }

    // The supported op-api cat filters empty 1-D inputs and returns an empty
    // tensor without dispatch. This branch is chosen by shape, never by cost.
    auto plan = generate_cpu_control(program.main_lane, "target empty prefix concatenation");
    const auto costs = cost::estimate_host_operation(operator_provider_, operation, false);
    std::pair<double, double> remainder{};
    bind_cpu_control(plan, costs.main_us, costs.main_residual_us, remainder);
    return plan;
}

HiCacheHostExpansion HiCacheWriteCalls::load_admission_tail(const LoadAdmissionProgram & program, uint64_t pages, bool allocated) const {
    auto plan = generate_cpu_control(program.main_lane, "target load admission bookkeeping");
    structure::OperationWork work{
        "hicache_io_control",
        allocated ? "load.index.tail" : "load.index.failure_tail",
        "ascend.paged_unsorted",
        "cpu_service",
        { { "pages", static_cast<double>(pages) },
          { "page_size_tokens", static_cast<double>(page_size_) },
          { "requested_tokens", static_cast<double>(core::checked_multiply_u64(pages, page_size_, "Load token count overflow")) } }
    };
    const auto costs = cost::estimate_host_operation(operator_provider_, work, false);
    std::pair<double, double> remainder{};
    bind_cpu_control(plan, costs.main_us, costs.main_residual_us, remainder);
    return plan;
}

void HiCacheWriteCalls::start_load_branch(size_t node, uint64_t time, simulation::FutureDag & future) {
    const auto & branch = empty_load_branches_.at(node);
    const auto & item = replay_.fact(branch.owner);
    if (!item.started) throw std::runtime_error("Host-load branch precedes its lookup input");
    const auto * work = replay_.state().load_admission_work(item.fact);
    if (!work) {
        auto inactive = branch.original;
        if (inactive.nodes.empty()) {
            const auto & call = capacity_calls_.at(branch.owner);
            inactive = generate_cpu_control(call.lane, "target no-load return");
            const structure::OperationWork operation{
                .component = "execution_control",
                .operation = "load.noop_return",
                .implementation = "no_host_hit",
                .timing_scope = "cpu_service",
            };
            const auto costs = cost::estimate_host_operation(operator_provider_, operation, false);
            std::pair<double, double> remainder{};
            bind_cpu_control(inactive, costs.main_us, costs.main_residual_us, remainder);
        }
        const auto restored = expand_host(inactive, {}, future);
        future.depend(restored.host_return, branch.continuation);
        return;
    }
    auto & call = capacity_calls_.at(branch.owner);
    auto fact = item.fact;
    fact.ts = time;
    fact.execution_anchor_node_id = node;

    if (work->skipped_below_threshold) {
        const auto & program = load_admissions_[select_load_admission(fact.pid, fact.tid)];
        auto skipped = generate_cpu_control(call.lane, "target threshold skip: no allocator or DMA");
        structure::OperationWork operation{
            "hicache_io_control",
            "load.admission.skip",
            "below_token_threshold",
            "cpu_service",
            { { "pages", static_cast<double>(work->requested_pages) },
              { "page_size_tokens", static_cast<double>(page_size_) },
              { "requested_tokens", static_cast<double>(core::checked_multiply_u64(work->requested_pages, page_size_, "Load token count overflow")) },
              { "promoted_nodes", static_cast<double>(work->node_pages.size()) } }
        };
        const auto costs = cost::estimate_host_operation(operator_provider_, operation, false);
        std::pair<double, double> remainder{};
        bind_cpu_control(skipped, costs.main_us, costs.main_residual_us, remainder);
        HiCacheHostSequence sequence(call.positions, stream_insertions_, future, node);
        (void)sequence.append(skipped, {});
        const auto prefix = sequence.append(load_prefix_concat(program, *work),
                                            work->existing_prefix_pages ? std::span<const size_t>{ program.prefix_concat.lanes } : std::span<const size_t>{});
        future.depend(prefix.host_return, branch.continuation);
        return;
    }

    const auto attempts = replay_.state().load_allocation_attempts(fact);
    const bool allocated = attempts.back().succeeds;
    const bool evict = replay_.state().eviction_work(fact) != nullptr;
    const auto selected = select_load_admission(fact.pid, fact.tid);
    const auto & program = load_admissions_[selected];

    auto preparation_plan = generate_cpu_control(call.lane, "target host load preparation");
    structure::OperationWork preparation{
        "hicache_io_control",
        "load.admission.prepare",
        "host_indices_cpu",
        "cpu_service",
        { { "pages", static_cast<double>(work->requested_pages) },
          { "page_size_tokens", static_cast<double>(page_size_) },
          { "requested_tokens", static_cast<double>(core::checked_multiply_u64(work->requested_pages, page_size_, "Load token count overflow")) },
          { "promoted_nodes", static_cast<double>(work->node_pages.size()) } }
    };
    const auto preparation_cost = cost::estimate_host_operation(operator_provider_, preparation, false);
    std::pair<double, double> preparation_remainder{};
    bind_cpu_control(preparation_plan, preparation_cost.main_us, preparation_cost.main_residual_us, preparation_remainder);
    HiCacheHostSequence entry(call.positions, stream_insertions_, future, node);
    const auto prepared = entry.append(preparation_plan, {}).host_return;
    std::optional<size_t> previous = prepared;

    const auto merge = [&](const model::HiCacheLoadAllocatorAttempt & attempt) {
        if (!attempt.merge_release) return;

        const auto pages = core::checked_add_u64(attempt.free_pages, attempt.release_pages, "Load merge work overflow");
        structure::OperationWork operation{
            "hicache_io_control",
            "",
            "ascend.paged_sorted",
            "cpu_service",
            { { "free_pages", static_cast<double>(attempt.free_pages) },
              { "release_pages", static_cast<double>(attempt.release_pages) },
              { "pages", static_cast<double>(pages) },
              { "page_size_tokens", static_cast<double>(page_size_) } }
        };
        HiCacheHostSequence sequence(call.positions, stream_insertions_, future, previous);
        for (const auto * step : { &program.merge_concat, &program.merge_sort }) {
            operation.operation = "load.index." + step->operation;
            previous = sequence.append(bind_load_operation(*step, operation), step->lanes).host_return;
        }

        // Clearing the release queue allocates an empty tensor; it is CPU
        // bookkeeping, not another device sort or a wait for sort completion.
        auto cleared = generate_cpu_control(call.lane, "target clear merged release queue");
        operation.operation = "load.index.merge_tail";
        const auto costs = cost::estimate_host_operation(operator_provider_, operation, false);
        std::pair<double, double> remainder{};
        bind_cpu_control(cleared, costs.main_us, costs.main_residual_us, remainder);
        previous = sequence.append(cleared, {}).host_return;
    };
    merge(attempts.front());

    if (evict || !allocated) {
        auto failed = generate_cpu_control(call.lane, "target failed allocation");
        const structure::OperationWork failure{
            .component = "execution_control",
            .operation = "load.allocation_failure",
            .implementation = allocator_need_sort_ ? "paged_sorted" : "paged_unsorted",
            .timing_scope = "thread_cpu_service",
            .features = {
                {"requested_tokens", static_cast<double>(core::checked_multiply_u64(work->requested_pages, page_size_, "Load token count overflow"))},
                {"page_size_tokens", static_cast<double>(page_size_)},
            },
        };
        const auto duration = operator_provider_.estimate(failure).duration_us;
        std::pair<double, double> remainder{};
        bind_cpu_control(failed, duration, 0, remainder);
        load_failure_templates_.insert_or_assign(branch.owner, std::move(failed));
    }
    if (evict) {
        previous = expand_capacity(call, fact, future, previous);
        if (!previous) throw std::logic_error("Target load retry has no capacity return");

        // Eviction completion precedes the retry; it does not publish a
        // successful allocation before its actual allocator return.
        allocation_returns_.erase(*previous);
        merge(attempts.back());
    }

    HiCacheHostSequence allocation(call.positions, stream_insertions_, future, previous);
    auto returned = node;
    if (allocated) {
        for (const auto & step : program.allocation) returned = allocation.append(cost_load_step(step, work->requested_pages), step.lanes).host_return;
    }
    else {
        // SGLang retries controller.load after eviction even when capacity
        // remains insufficient. expand_capacity charged the first failure;
        // this is the second attempt, not post-return bookkeeping.
        returned = allocation.append(load_failure_templates_.at(branch.owner), {}).host_return;
    }

    future.depend(returned, branch.continuation);
    allocation_returns_[returned].push_back(branch.owner);
    load_branch_returns_.emplace(returned, LoadBranchReturn{ branch.owner, selected, branch.continuation, allocated });
}

} // namespace markov::trace_graph::modules::hicache::runtime
