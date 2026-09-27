#pragma once
#include "markov/trace_graph/core/dag_graph.hpp"
#include <string>

namespace markov::trace_graph::cli {
void prepare_cpu_service(core::DagGraph&, const std::string& manifest,
                         const std::string& measurements, const std::string& output);
}
