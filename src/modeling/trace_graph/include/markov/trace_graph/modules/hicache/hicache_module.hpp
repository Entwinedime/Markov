/**
 * @file
 * @brief SimulationModule adapter for the HiCache state model.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/model/state.hpp"
#include "markov/trace_graph/modules/module.hpp"

#include <memory>

namespace markov::trace_graph::modules::hicache {

/**
 * @brief Adapts HiCache state replay to the ordered module pipeline.
 *
 * The adapter owns configuration and result publication only. Canonical state and policy
 * transitions remain in `HiCacheState` so diagnostics and pipeline plumbing cannot become
 * additional state sources.
 */
class HiCacheModule final : public SimulationModule {
public:
    explicit HiCacheModule(frontend::HiCacheConfig config);
    HiCacheModule(frontend::HiCacheConfig config, std::shared_ptr<model::HiCacheModelResult> result);

    /** @brief Returns the stable registry and diagnostics name. */
    [[nodiscard]] std::string_view name() const noexcept override;

    /** @brief Extracts approved HiCache facts and executes canonical target-state replay. */
    void apply(core::DagGraph & graph) override;

    /** @brief Reports whether apply() produced a diagnostics summary. */
#ifdef DEBUG
    [[nodiscard]] bool has_summary() const override;
#endif

    /** @brief Returns the complete effect-decision ledger in Release and Debug builds. */
    [[nodiscard]] const model::HiCacheEffectDecisionLedger & effect_decisions() const { return result_->effect_decisions; }

    /** @brief Returns the diagnostics summary produced by the latest successful apply. */
#ifdef DEBUG
    [[nodiscard]] const model::HiCacheSummary & summary() const { return result_->summary; }
#endif

private:
    frontend::HiCacheConfig config_;
    std::shared_ptr<model::HiCacheModelResult> result_;
#ifdef DEBUG
    bool applied_ = false;
#endif
};

} // namespace markov::trace_graph::modules::hicache
