/**
 * @file
 * @brief HiCache SimulationModule adapter implementation.
 */
#include "markov/trace_graph/modules/hicache/hicache_module.hpp"

#include <stdexcept>
#include <string_view>
#include <utility>

namespace markov::trace_graph::modules::hicache {

namespace hicache_module_detail {

constexpr std::string_view kModuleName = "HiCacheModule";

} // namespace hicache_module_detail

using hicache_module_detail::kModuleName;

HiCacheModule::HiCacheModule(frontend::HiCacheConfig config) : HiCacheModule(std::move(config), std::make_shared<model::HiCacheModelResult>()) {}

HiCacheModule::HiCacheModule(frontend::HiCacheConfig config, std::shared_ptr<model::HiCacheModelResult> result)
    : config_(std::move(config)),
      result_(std::move(result)) {
    if (!result_) throw std::invalid_argument("HiCacheModule requires a shared model result");
}

std::string_view HiCacheModule::name() const noexcept { return kModuleName; }

/**
 * @brief Executes HiCache state replay as one pipeline module.
 *
 * This adapter does not edit graph topology or durations. A following patch module consumes
 * the immutable replay result, while Debug summaries are serialized only at the CLI
 * diagnostics boundary.
 */
void HiCacheModule::apply(core::DagGraph & graph) {
    *result_ = model::apply_hicache_model(graph, config_);
#ifdef DEBUG
    applied_ = true;
#endif
}

#ifdef DEBUG
bool HiCacheModule::has_summary() const { return applied_; }
#endif

} // namespace markov::trace_graph::modules::hicache
