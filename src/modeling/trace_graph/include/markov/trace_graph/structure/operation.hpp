#pragma once

#include <map>
#include <string>

namespace markov::trace_graph::structure {

/** Execution work, independent of a numerical model or observation source.
 * Features use explicit units: tokens, pages, bytes and dimensionless counts.
 * An implementation identifies an operator, never an experiment or workload.
 */
struct OperationWork {
    std::string component;
    std::string operation;
    std::string implementation;
    std::string timing_scope;
    std::map<std::string, double> features;
};

/** A migration candidate is keyed by its corresponding source node. Unchanged
 * work may retain that node's cost; changed work requires a structural owner
 * to replace it before binding. This is not a model request or proxy geometry.
 */
struct CostMigration {
    std::string operation;
    std::string implementation;
    bool unchanged;
};

} // namespace markov::trace_graph::structure
