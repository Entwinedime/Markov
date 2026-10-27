#pragma once

#include "markov/trace_graph/structure/tensor_program.hpp"

#include <string>
#include <vector>

namespace markov::trace_graph::adapters::sglang {

/** Lower recorded or target dense work using current SGLang eager/Ascend
 * execution rules. Geometry is supplied by work, never by a cost response. */
[[nodiscard]] std::vector<structure::TensorOperation> dense_tensor_program(const std::string & component, const std::string & implementation,
                                                                           const structure::DenseShape & shape);

} // namespace markov::trace_graph::adapters::sglang
