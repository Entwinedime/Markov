#pragma once

#include "markov/performance/measured_cost_model.hpp"

namespace markov {

using CommunicationCostModel = MeasuredCostModel<CommunicationCostFeatures, CostQueryKind::Communication, NodeCostBasis::CommunicationCandidate>;

} // namespace markov
