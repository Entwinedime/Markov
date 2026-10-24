#pragma once

#include <cstdint>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/controller/serving.hpp"
#include "markov/execution/executor.hpp"
#include "markov/templates/batch_expansion.hpp"

namespace markov {

// Assemble graph effects before committing their corresponding controller state changes.
class ServingWorkflow {
public:
    ServingWorkflow(ServingSettings settings, std::vector<Request> requests, BatchExpansion & expansion, const NodeDurations & durations);

    nlohmann::json run(bool details);

private:
    nlohmann::json describe_batch(const Batch & batch) const;
    void record_publication(nlohmann::json & result, const BatchEffects & effects, std::int64_t time_ns, bool details) const;
    void record_completion(nlohmann::json & result, nlohmann::json & decision, const BatchEffects & effects, std::int64_t time_ns, bool details) const;

    ServingSettings settings_;
    ServingController controller_;
    BatchExpansion & expansion_;
    Executor executor_;
};

} // namespace markov
