#include "markov/commands/serial_prediction.hpp"
#include "markov/commands/costs.hpp"
#include "markov/commands/trajectory.hpp"
#include "markov/trace_graph/simulation/program_execution.hpp"

#include <algorithm>
#include <chrono>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <sys/resource.h>
#include <variant>
#include <vector>

namespace markov::commands {

nlohmann::json serial_prediction_report(const adapters::PreparedPrediction & input) {
    using namespace trace_graph;
    using Json = nlohmann::json;
    using Clock = std::chrono::steady_clock;
    const auto & source = input.source.context;
    const auto & observed = source.observations();
    cost::Queries costs(input.costs.models, &source);
    auto cells = Json::array();
    double maximum_input = 0, maximum_output = 0;
    for (const auto & target : std::get<std::vector<adapters::SerialPredictionTarget>>(input.targets)) {
        const auto begin = Clock::now();
        planning::SerialProgram program(target.requests);
        simulation::ProgramResult result;
        if (!target.infeasible) result = simulation::ProgramExecution(program, costs, program.resource_count, input.node_scale, input.record_graph).run();
        const bool complete = !target.infeasible && result.cost_gaps.empty() && program.complete();
        auto requests = Json::array();
        std::optional<graph::Time> formal_begin, formal_end;
        for (std::size_t id = 0; id < target.requests.size(); ++id) {
            const auto & port = program.ports()[id];
            const auto & control = target.requests[id].work.front().features;
            maximum_input = std::max(maximum_input, control.at("input_tokens"));
            maximum_output = std::max(maximum_output, control.at("decode_steps") + 1);
            const auto start = complete ? std::optional(result.execution.starts_ns[port.start]) : std::nullopt;
            const auto end = complete ? std::optional(result.execution.completions_ns[port.response]) : std::nullopt;
            if (complete && observed.requests[id].formal) {
                if (!formal_begin) formal_begin = start;
                formal_end = end;
            }
            requests.push_back({
                {       "request",                         observed.requests[id].identity },
                {        "formal",                           observed.requests[id].formal },
                {      "start_ns",                   start ? Json(*start) : Json(nullptr) },
                {   "response_ns",                       end ? Json(*end) : Json(nullptr) },
                {      "http_sec", complete ? Json((*end - *start) / 1e9) : Json(nullptr) },
                {  "input_tokens",                             control.at("input_tokens") },
                { "prefill_steps",                            control.at("prefill_steps") },
                {  "decode_steps",                             control.at("decode_steps") }
            });
            if (result.graph) {
                requests.back()["start_node"] = result.graph->operation_nodes[port.start].value();
                requests.back()["response_node"] = result.graph->operation_nodes[port.response].value();
            }
        }
        auto gaps = Json::array();
        for (const auto & gap : result.cost_gaps) {
            Json item{
                {            "status", cost_status_name(gap.response.status) },
                { "affected_features",        gap.response.affected_features }
            };
            if (const auto * work = std::get_if<planning::Work>(&gap.work)) item["work"] = work_json(*work);
            gaps.push_back(std::move(item));
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
        cells.push_back({
            {                  "name",                                                  target.name                                      },
            {         "configuration",                                                                               target.configuration },
            {                "status",
             target.infeasible ? "resource_infeasible"
             : complete        ? "predicted"
             : "missing_costs"                                                                                                           },
            {                 "issue",                                       target.infeasible ? Json(*target.infeasible) : Json(nullptr) },
            {             "cost_gaps",                                                                                    std::move(gaps) },
            {         "e2e_predicted",                                                                                           complete },
            {       "formal_http_sec", complete && formal_begin && formal_end ? Json((*formal_end - *formal_begin) / 1e9) : Json(nullptr) },
            { "background_finish_sec",           complete ? Json(std::ranges::max(result.execution.completions_ns) / 1e9) : Json(nullptr) },
            {       "generated_nodes",                                                                     result.execution.states.size() },
            {        "executed_nodes",                                                                          result.execution.executed },
            {       "generated_edges",                                                                             result.execution.edges },
            {              "coverage",                                                                                std::move(coverage) },
            {            "node_scale",                                                                                 std::move(scaling) },
            {              "requests",                                                                                std::move(requests) },
            {        "simulation_sec",                                        std::chrono::duration<double>(Clock::now() - begin).count() }
        });
        if (result.graph) cells.back()["execution_graph"] = generated_graph_report(result);
    }
    std::set<std::string> sources, assumptions;
    for (const auto id : costs.limited_models()) {
        const auto & model = input.costs.models.model(id);
        sources.insert(model.sources.begin(), model.sources.end());
        assumptions.insert(model.limitations.begin(), model.limitations.end());
    }
    rusage usage{};
    const auto usage_status = getrusage(RUSAGE_SELF, &usage);
    return {
        { "deployment", input.costs.deployment },
        { "status", std::ranges::all_of(cells, [](const Json & cell) { return cell.at("e2e_predicted").get<bool>(); }) ? "completed" : "incomplete" },
        { "manifest", input.source.manifest.path.string() },
        { "models", input.models_path.string() },
        { "source_prepare_sec", input.source.read_and_interpret_sec + input.source.build_sec },
        { "source_records_read", std::accumulate(input.source.input_record_counts.begin(), input.source.input_record_counts.end(), std::size_t{ 0 }) },
        { "unread_device_trace_files", input.source.skipped_files.size() },
        { "source_details_materialized", source.detailed_preparation_sec() != 0 },
        { "process_peak_rss_bytes", usage_status == 0 ? Json(usage.ru_maxrss * 1'024) : Json(nullptr) },
        { "selected_models", input.costs.models.size() },
        { "model_sources", sources },
        { "cost_assumptions", assumptions },
        { "sampling_context", { { "max_input_tokens", maximum_input }, { "max_new_tokens", maximum_output } } },
        { "cells", std::move(cells) },
        { "limitations",
         { "Closed synchronous requests on the declared single-device deployment, fresh KV per request; no serving overlap or offload model.",
            "Target phases come from workload and chunking rules. Changed phase and controller costs use independent data; target observations are not read.",
            "Controller remainder is grouped before phases: no external intermediate consumers exist in this supported execution.",
            "Serial client pacing retains the source interval between requests, outside each request's HTTP. Changed client behavior is not modeled.",
            "Source HTTP transport and outer handler intervals remain explicit residual estimates; changed frontend behavior and profiler effects are "
            "uncertain.",
            "First-phase and first-request flags are aggregate estimates, not a complete compilation/cache model. Execution does not certify E2E "
            "accuracy." } }
    };
}

} // namespace markov::commands
