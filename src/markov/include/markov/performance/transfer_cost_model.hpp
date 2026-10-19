#pragma once

#include "markov/performance/measured_cost_model.hpp"

namespace markov {

using TransferCostModel = MeasuredCostModel<TransferCostFeatures, CostQueryKind::Transfer, NodeCostBasis::TransferCandidate>;

} // namespace markov
