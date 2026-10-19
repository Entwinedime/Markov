#pragma once

#include "markov/performance/measured_cost_model.hpp"

namespace markov {

using DeviceCommandCostModel = MeasuredCostModel<DeviceCommandWork, CostQueryKind::DeviceCommand, NodeCostBasis::DeviceCommandCandidate>;

} // namespace markov
