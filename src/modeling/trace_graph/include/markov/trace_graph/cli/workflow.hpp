/**
 * @file
 * @brief Public CLI orchestration boundary for one TraceGraph backend run.
 */
#pragma once

#include "markov/trace_graph/core/client_requests.hpp"
#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/io/trace_manifest_input.hpp"

#include <memory>
#include <nlohmann/json.hpp>

namespace markov::trace_graph::core {
class Logger;
}

namespace markov::trace_graph::cli {

struct CliOptions;
class ModulePipeline;

/** Source preparation owns observed coordinates and request anchors.
 * Raw replay precedes CPU correction. Each target consumes a private instance;
 * the parent of a prediction batch never mutates its prepared source. */
struct PreparedSource {
    core::DagGraph graph;
    io::ManifestClientInput client_input;
    core::ClientRequestChain client_chain;
    nlohmann::json replay_report;
    std::unique_ptr<const core::DagGraph> migration_graph{};
};

[[nodiscard]] PreparedSource prepare_source(const CliOptions & options, bool include_preparation = false);

/** Mutate one private source in place; its caller owns both graph lifetimes. */
int run_prepared_workflow(const CliOptions & options, ModulePipeline & modules, PreparedSource & source, core::Logger & logger);

/** Linux container batch: prepare once, then fork bounded target executions.
 * Completion records go to stdout; each target retains its own log. */
int run_prediction_batch(const CliOptions & options, core::Logger & logger);

/**
 * @brief Executes trace loading, DAG construction, modules, simulation, and output.
 *
 * User-input and runtime failures propagate as exceptions to `main`; successful
 * execution returns zero after the required run summary has been written.
 */
int run_workflow(const CliOptions & options, core::Logger & logger);

} // namespace markov::trace_graph::cli
