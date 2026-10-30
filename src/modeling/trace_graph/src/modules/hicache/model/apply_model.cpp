/**
 * @file
 * @brief Target phase work and source operator identities, without cost estimation.
 */
#include "markov/trace_graph/modules/hicache/model/replay.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::model {

HiCacheModelResult HiCacheModelReplay::finish() {
    if (finished_) throw std::logic_error("HiCache replay already finished");
    if (std::ranges::any_of(facts_, [](const auto & item) { return !item.consumed; }))
        throw std::logic_error("HiCache replay cannot finish before all formal facts execute");

    finished_ = true;
    if (result_.effect_decisions.status == "needs_calibration") return result_;
    if (!execution_prefetch_control_) state_.finalize();

    (void)current_phase_work();
    auto result = result_;
    auto effects = state_.effect_decision_ledger();
    for (const auto & [reason, count] : result.effect_decisions.missing_facts)
        effects.missing_facts[reason] = core::checked_add_u64(effects.missing_facts[reason], count, "HiCache missing-fact count overflow");

    if (!effects.missing_facts.empty()) effects.status = "partial";
    result.effect_decisions = std::move(effects);
    result.replay_complete = true;
    return result;
}

const HiCachePhaseWorkLedger & HiCacheModelReplay::current_phase_work() {
    auto & work = result_.phase_work;
    if (result_.effect_decisions.status == "needs_calibration") return work;

    const auto & prefills = state_.prefill_work_items();
    const auto & allocations = state_.allocator_work_items();
    const auto admitted = work.prefills.size();
    work.prefills.insert(work.prefills.end(), prefills.begin() + admitted, prefills.end());
    work.allocator_calls.insert(work.allocator_calls.end(), allocations.begin() + work.allocator_calls.size(), allocations.end());

    // Source handles describe the current execution skeleton, not costs.
    // Numerical provenance is read only by the later cost binding stage.
    for (size_t i = admitted; i < work.prefills.size(); ++i) {
        auto & prefill = work.prefills[i];
        const auto source_position = source_phase_positions_.find({ prefill.pid, prefill.request_id });
        if (source_position == source_phase_positions_.end()) {
            ++work.blockers["source_phase_request_missing"];
            continue;
        }

        const auto & source = source_input_->phases.observations[source_position->second];
        prefill.logical_input = source.logical_input;
        prefill.common_kernel_nodes = source.prefill_common_kernel_node_ids;
        prefill.prefix_attention_nodes = source.prefill_prefix_attention_node_ids;
        prefill.collective_nodes = source.prefill.collective_node_ids;
        prefill.submit_nodes = source.prefill.submit_cpu_node_ids;

        HiCacheDecodeWorkItem decode{
            .logical_input = source.logical_input,
            .pid = source.pid,
            .request_id = prefill.request_id,
            .prompt_token_count = prefill.prompt_token_count,
            .target_page_size = prefill.target_page_size,
            .iteration_count = source.decode_iteration_count,
            .node_iterations = source.decode_node_iterations,
            .kernel_nodes = source.decode_kernel_node_ids,
            .collective_nodes = source.decode.collective_node_ids,
            .submit_nodes = source.decode.submit_cpu_node_ids,
        };
        work.decodes.push_back(std::move(decode));
    }

    work.prefill_status =
        source_input_->phases.ready() && work.prefills.size() == source_input_->phases.observations.size() && work.blockers.empty() ? "ready" : "not_ready";
    work.decode_status = work.decodes.size() == work.prefills.size() && work.blockers.empty() ? "ready" : "not_ready";
    work.status = work.prefill_status == "ready" && work.decode_status == "ready" ? "ready" : "not_ready";
    return work;
}

} // namespace markov::trace_graph::modules::hicache::model
