#include "markov/commands/prediction.hpp"

#include "markov/commands/costs.hpp"
#include "markov/commands/trajectory.hpp"
#include "markov/trace_graph/simulation/program_execution.hpp"

#include <algorithm>
#include <chrono>
#include <numeric>
#include <set>
#include <sys/resource.h>

namespace markov::commands {

nlohmann::json prediction_report(const adapters::ktransformers::PreparedPrediction & input) {
    using namespace trace_graph;
    using Json = nlohmann::json;
    const auto & source = input.source.context;
    const auto & observations = source.observations();
    auto & selected = input.costs.at(1);
    cost::Queries costs(selected.models, &source);
    auto cells = Json::array();
    for (const auto & target : input.targets) {
        const auto begin = std::chrono::steady_clock::now();
        adapters::ktransformers::Program program(input, target);
        auto result = simulation::ProgramExecution(program, costs, 1, input.node_scale, input.record_graph).run();
        const bool complete = program.complete() && result.cost_gaps.empty();
        auto requests = Json::array();
        std::optional<graph::Time> formal_begin, formal_end;
        for (std::size_t id = 0; id < program.requests().size(); ++id) {
            const auto & request = program.requests()[id];
            Json row{
                {     "request", observations.requests[id].identity },
                {      "formal",   observations.requests[id].formal },
                {    "start_ns",                            nullptr },
                { "response_ns",                            nullptr },
                {    "http_sec",                            nullptr }
            };
            if (request.completed) {
                const auto start = result.execution.starts_ns[*request.start];
                const auto end = result.execution.completions_ns[*request.response];
                row.update({
                    {    "start_ns",               start },
                    { "response_ns",                 end },
                    {    "http_sec", (end - start) / 1e9 }
                });
                if (observations.requests[id].formal) {
                    if (!formal_begin) formal_begin = start;
                    formal_end = end;
                }
                if (result.graph) {
                    row["start_node"] = result.graph->operation_nodes[*request.start].value();
                    row["response_node"] = result.graph->operation_nodes[*request.response].value();
                }
            }
            requests.push_back(std::move(row));
        }
        auto gaps = Json::array();
        for (const auto & gap : result.cost_gaps) {
            Json entry{
                {            "status", cost_status_name(gap.response.status) },
                { "affected_features",        gap.response.affected_features }
            };
            if (const auto * work = std::get_if<planning::Work>(&gap.work)) entry["work"] = work_json(*work);
            gaps.push_back(std::move(entry));
        }
        Json coverage = Json::object();
        for (const auto & [kind, count] : result.coverage) coverage[cost_coverage_name(kind)] = count;
        auto scaling = Json::array();
        for (std::size_t id = 0; id < input.node_scale.size(); ++id)
            scaling.push_back({
                {          "name",                                   input.node_scale[id].name },
                {        "factor",                                 input.node_scale[id].factor },
                { "matched_nodes", result.scale_matches.empty() ? 0 : result.scale_matches[id] }
            });
        const Json sampling{
            { "configuration", target.configuration }
        };
        cells.push_back({
            {                  "name",                                                  target.name                                      },
            {            "deployment",                                                                                   input.deployment },
            {         "configuration",                                                                               target.configuration },
            {           "work_layout",
             { { "cpu_threads", target.cpu_threads },
             { "numa_pools", target.numa_pools },
             { "resident_experts_per_layer", target.resident_experts },
             { "routing", "Base per-layer expert histograms; target routing is not read" },
             { "region_scope", "Closed serial MoE submit/compute/join response includes internal CPU/NPU overlap" } }                    },
            {      "sampling_context",                                                                                           sampling },
            {                "status",                                                           complete ? "predicted" : "missing_costs" },
            {         "e2e_predicted",                                                                                           complete },
            {       "formal_http_sec", complete && formal_begin && formal_end ? Json((*formal_end - *formal_begin) / 1e9) : Json(nullptr) },
            { "background_finish_sec",                                     complete ? Json(result.work_finished_ns / 1e9) : Json(nullptr) },
            {             "cost_gaps",                                                                                    std::move(gaps) },
            {              "coverage",                                                                                std::move(coverage) },
            {            "node_scale",                                                                                 std::move(scaling) },
            {       "generated_nodes",                                                                     result.execution.states.size() },
            {        "executed_nodes",                                                                          result.execution.executed },
            {       "generated_edges",                                                                             result.execution.edges },
            {              "requests",                                                                                std::move(requests) },
            {        "simulation_sec",                    std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() }
        });
        if (result.graph) cells.back()["execution_graph"] = generated_graph_report(result);
    }
    std::set<std::string> sources, assumptions;
    for (const auto id : costs.limited_models()) {
        const auto & model = selected.models.model(id);
        sources.insert(model.sources.begin(), model.sources.end());
        assumptions.insert(model.limitations.begin(), model.limitations.end());
    }
    rusage usage{};
    const auto usage_status = getrusage(RUSAGE_SELF, &usage);
    return {
        { "deployment", input.deployment },
        { "status", std::ranges::all_of(cells, [](const Json & cell) { return cell.at("e2e_predicted").get<bool>(); }) ? "completed" : "incomplete" },
        { "manifest", input.source.manifest.path.string() },
        { "models", { { "1", input.models_paths.at(1).string() } } },
        { "source_prepare_sec", input.source.read_and_interpret_sec + input.source.build_sec },
        { "source_records_read", std::accumulate(input.source.input_record_counts.begin(), input.source.input_record_counts.end(), std::size_t{}) },
        { "unread_low_level_trace_files", input.source.skipped_files.size() },
        { "source_details_materialized", false },
        { "process_peak_rss_bytes", usage_status == 0 ? Json(usage.ru_maxrss * 1'024) : Json(nullptr) },
        { "selected_models", selected.models.size() },
        { "model_sources", sources },
        { "cost_assumptions", assumptions },
        { "cells", std::move(cells) },
        { "limitations",
         { "Single-NPU eager DeepSeek-V4, prefix expert placement, serial requests without scheduler overlap, radix reuse or deferred experts.",
            "Each MoE region is closed at native submit/combine ports. Independent closed responses include internal CPU/NPU overlap, not a sum of separate "
            "services.",
            "The target uses base routing; numerical changes under different expert placement can change future routing and require independent qualification.",
            "Unchanged non-MoE work and frontend intervals retain source responses or residual approximations; no gap, unknown control or HTTP time is "
            "deleted.",
            "Semantic probe and event overhead has not yet been calibrated; semantic execution is not detailed source replay or normal accuracy "
            "evidence." } }
    };
}

} // namespace markov::commands
