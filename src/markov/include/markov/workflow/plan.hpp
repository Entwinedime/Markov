#pragma once

#include "markov/workflow/project.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace markov {

class TemplateAdapter;
class CollectionAdapter;

class Planner {
public:
    Planner(std::span<const TemplateAdapter * const> adapters, std::span<const CollectionAdapter * const> collection_adapters)
        : adapters_(adapters),
          collection_adapters_(collection_adapters) {}

    nlohmann::json describe(const Project & project) const;
    nlohmann::json collection_plan(const Project & project) const;
    nlohmann::json prepare_prediction(const Project & project) const;
    nlohmann::json prepare_performance(const Project & project, const nlohmann::json & prediction) const;

    // Explicit diagnostic acquisition. Exploration chooses its own representative
    // inputs; it must not call this once per target workload as a coverage fallback.
    nlohmann::json prepare_collection(const Project & project, const Candidate & configuration, std::span<const Request> requests) const;
    nlohmann::json prepare_representative(const Project & project, const Candidate & configuration) const;
    nlohmann::json prepare_target(const Project & project, const Candidate & configuration, const RequestWorkload & workload, std::int64_t budget_sec,
                                  std::string_view purpose) const;
    nlohmann::json prepare_sampling(const Project & project, const Candidate & configuration, const RequestWorkload & prompts,
                                    const nlohmann::json & measurement_requests) const;
    nlohmann::json prepare_communication(const Project & project, const Candidate & configuration) const;

private:
    const CollectionAdapter & collection_adapter(Framework framework) const;
    nlohmann::json prepare_inputs(const Project & project, const Candidate & configuration) const;

    // Both adapter lists are owned by the entry point throughout preparation.
    std::span<const TemplateAdapter * const> adapters_;
    std::span<const CollectionAdapter * const> collection_adapters_;
};

} // namespace markov
