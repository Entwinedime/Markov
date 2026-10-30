#pragma once

#include "markov/adapters/prediction.hpp"

#include <vector>

namespace markov::adapters::ktransformers {

std::vector<SerialPredictionTarget> prepare_targets(const nlohmann::json & request, const nlohmann::json & capture,
                                                    const trace_graph::source::SourceContext & source);

} // namespace markov::adapters::ktransformers
