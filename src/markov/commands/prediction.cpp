#include "markov/commands/prediction.hpp"
#include "markov/commands/costs.hpp"
#include "markov/trace_graph/planning/cache_program.hpp"
#include "markov/trace_graph/simulation/program_execution.hpp"

#include <algorithm>
#include <chrono>
#include <numeric>
#include <set>
#include <sys/resource.h>

namespace markov::commands {

nlohmann::json prediction_report(const adapters::PreparedPrediction & input) {
    using namespace trace_graph;
    using Json = nlohmann::json;
    using Clock = std::chrono::steady_clock;
    const auto & source = input.source.context;
    const auto & observations = source.observations();
    const auto backend = adapters::sglang::cache_backend(source);
    cost::Queries costs(input.costs.models, &source);
    auto cells = Json::array();
    for (const auto & target : input.targets) {
        const auto begin = Clock::now();
        planning::CacheProgram program(source,
                                       target.page_tokens,
                                       { target.cache.device_pages, target.cache.host_pages, backend.layers, target.cache.backup_after_hits },
                                       backend,
                                       target.cache.storage,
                                       input.initial,
                                       planning::RegionBackend{ "sglang.ascend.eager", target.result_implementation, target.maximum_prefill_tokens });
        simulation::ProgramResult result;
        if (!program.issue()) result = simulation::ProgramExecution(program, costs, program.resources()).run();
        const bool complete =
            !program.issue() && result.cost_gaps.empty() && std::ranges::all_of(program.requests(), [&](const planning::CacheRequestResult & request) {
                return request.completed && request.response && result.execution.states[*request.response] == simulation::OperationState::Complete;
            });
        auto requests = Json::array();
        std::optional<graph::Time> formal_begin, formal_end;
        for (std::size_t id = 0; id < program.requests().size(); ++id) {
            const auto & request = program.requests()[id];
            const bool done = request.completed && request.start && request.response;
            const auto start = done ? std::optional(result.execution.starts_ns[*request.start]) : std::nullopt;
            const auto end = done ? std::optional(result.execution.completions_ns[*request.response]) : std::nullopt;
            if (done && observations.requests[id].formal) {
                if (!formal_begin) formal_begin = start;
                formal_end = end;
            }
            requests.push_back({
                {          "request",                 observations.requests[id].identity },
                {           "formal",                   observations.requests[id].formal },
                {         "start_ns",               start ? Json(*start) : Json(nullptr) },
                {      "response_ns",                   end ? Json(*end) : Json(nullptr) },
                {         "http_sec", done ? Json((*end - *start) / 1e9) : Json(nullptr) },
                { "device_hit_pages",                           request.device_hit_pages },
                {     "loaded_pages",                               request.loaded_pages },
                {   "prefill_tokens",                             request.prefill_tokens }
            });
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
        cells.push_back({
            {                  "name",                                                  target.name                                      },
            {         "configuration",                                                                               target.configuration },
            {                "status",
             complete          ? "predicted"
             : program.issue() ? "structure_unavailable"
             : "missing_costs"                                                                                                           },
            {                 "issue",                                           program.issue() ? Json(*program.issue()) : Json(nullptr) },
            {             "cost_gaps",                                                                                    std::move(gaps) },
            {         "e2e_predicted",                                                                                           complete },
            {       "formal_http_sec", complete && formal_begin && formal_end ? Json((*formal_end - *formal_begin) / 1e9) : Json(nullptr) },
            { "background_finish_sec",           complete ? Json(std::ranges::max(result.execution.completions_ns) / 1e9) : Json(nullptr) },
            {       "generated_nodes",                                                                     result.execution.states.size() },
            {        "executed_nodes",                                                                          result.execution.executed },
            {       "generated_edges",                                                                             result.execution.edges },
            {              "coverage",                                                                                std::move(coverage) },
            {    "backup_submissions",                                                                                  program.backups() },
            {      "load_submissions",                                                                                    program.loads() },
            {              "requests",                                                                                std::move(requests) },
            {        "simulation_sec",                                        std::chrono::duration<double>(Clock::now() - begin).count() }
        });
    }
    rusage usage{};
    const auto usage_status = getrusage(RUSAGE_SELF, &usage);
    std::set<std::string> assumptions, sources;
    for (const auto id : costs.limited_models()) {
        const auto & model = input.costs.models.model(id);
        assumptions.insert(model.limitations.begin(), model.limitations.end());
        sources.insert(model.sources.begin(), model.sources.end());
    }
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
        { "cells", std::move(cells) },
        { "limitations",
         { "Serial fixed-token eager requests on the declared deployment; target observations are not read.",
            "CPU and opaque stream responses overlap. Stream intervals include host starvation and internal communication; TP shares the latest ready input.",
            "First-phase and new-input-size terms estimate observed initialization conditions, not an exact compiler cache.",
            "Modeled parent waits use target readiness ports. Other source frontend/control intervals retain their opaque work and waits; unidentified "
            "probe overhead and changed control behavior remain uncertain.",
            "Cache query CPU retains the source residual approximation; state-dependent tree overhead and competing I/O/compute need further validation.",
            "HTTP send and post-send CPU are separate; unconsumed overlapped computation and background I/O still drain after HTTP.",
            "No normal E2E accuracy or complete device-causality qualification is implied by execution." } }
    };
}

} // namespace markov::commands
