#pragma once

#include <cstddef>
#include <map>
#include <optional>
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

/** A migration candidate is not a cost prediction request. A source reference
 * proves unchanged work; missing reference requires a structural owner to
 * replace the operation before costs can be bound. No proxy work features.
 */
struct CostMigration {
    std::string operation;
    std::string implementation;
    std::optional<size_t> source_node;
    std::string migration_basis;
};

} // namespace markov::trace_graph::structure
