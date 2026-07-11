/**
 * @file
 * @brief Phase 0/1 scaffold from HiCache effect intents to DAG mutations.
 */
#pragma once

#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/modules/hicache/model/result.hpp"
#include "markov/trace_graph/modules/module.hpp"

#include <memory>

namespace markov::trace_graph::modules::hicache {

/** @brief Business result for the HiCache patch plan and applied mutation journal. */
struct HiCacheDagPatchResult {
    std::string status = "not_applied";
    core::DagMutationPlan plan;
    core::DagMutationJournal journal;
#ifdef DEBUG
    core::DagTopologyValidationReport topology;
#endif
};

/**
 * @brief Consumes an immutable state-replay result and applies one validated DAG plan.
 *
 * Phase 0/1 deliberately emits an empty plan. Future phases must add behavior through
 * `DagMutationPlan`; direct node or edge edits would bypass atomic prevalidation.
 */
class HiCacheDagPatchModule final : public SimulationModule {
public:
    explicit HiCacheDagPatchModule(std::shared_ptr<const model::HiCacheModelResult> model_result);

    [[nodiscard]] std::string_view name() const noexcept override;
    void apply(core::DagGraph & graph) override;
#ifdef DEBUG
    [[nodiscard]] bool has_summary() const override;
#endif
    [[nodiscard]] const HiCacheDagPatchResult & result() const { return result_; }

private:
    std::shared_ptr<const model::HiCacheModelResult> model_result_;
    HiCacheDagPatchResult result_;
#ifdef DEBUG
    bool applied_ = false;
#endif
};

} // namespace markov::trace_graph::modules::hicache
