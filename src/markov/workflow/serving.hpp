#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

#include "markov/contracts/serving.hpp"
#include "markov/controller/control.hpp"
#include "markov/execution/executor.hpp"
#include "markov/templates/batch_expansion.hpp"

namespace markov {

// Assemble graph effects before committing their corresponding controller state changes.
class ServingWorkflow {
public:
    ServingWorkflow(ServingControl & controller, BatchExpansion & expansion, const NodeDurations & durations);

    nlohmann::json run(bool details);

private:
    nlohmann::json describe_batch(const Batch & batch) const;
    void record_publication(nlohmann::json & result, const BatchEffects & effects, std::int64_t time_ns, bool details) const;
    void record_completion(nlohmann::json & result, nlohmann::json & decision, const ServingAction & action, std::int64_t time_ns, bool details) const;

    // The caller owns the chosen control implementation for the entire workflow.
    ServingControl & controller_;
    BatchExpansion & expansion_;
    Executor executor_;
};

} // namespace markov
