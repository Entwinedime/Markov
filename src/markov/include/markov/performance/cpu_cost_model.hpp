#pragma once

#include "markov/performance/measured_cost_model.hpp"

namespace markov {

using CpuCostModel = MeasuredCostModel<CpuServiceWork, CostQueryKind::Cpu, NodeCostBasis::CpuCandidate>;

} // namespace markov
