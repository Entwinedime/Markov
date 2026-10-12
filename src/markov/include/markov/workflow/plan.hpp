#pragma once

#include "markov/contracts/project.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <span>

namespace markov {

class Planner {
public:
    nlohmann::json describe(const Project & project) const;
    nlohmann::json collection_plan(const Project & project) const;
    nlohmann::json prepare_prediction(const Project & project) const;

    // Explicit diagnostic acquisition. Exploration chooses its own representative
    // inputs; it must not call this once per target workload as a coverage fallback.
    nlohmann::json prepare_collection(const Project & project, const Candidate & configuration, std::span<const Request> requests) const;
    nlohmann::json prepare_representative(const Project & project, const Candidate & configuration) const;
    nlohmann::json prepare_target(const Project & project, const Candidate & configuration, const RequestWorkload & workload, std::int64_t budget_sec) const;
    nlohmann::json prepare_sampling(const Project & project, const Candidate & configuration, const RequestWorkload & prompts,
                                    const nlohmann::json & measurement_requests) const;
    nlohmann::json prepare_communication(const Project & project, const Candidate & configuration) const;
};

} // namespace markov
