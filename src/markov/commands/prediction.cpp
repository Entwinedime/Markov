#include "markov/commands/prediction.hpp"
#include "markov/commands/costs.hpp"
#include "markov/commands/trajectory.hpp"
#include "markov/trace_graph/planning/cache_program.hpp"
#include "markov/trace_graph/simulation/program_execution.hpp"

#include <algorithm>
#include <chrono>
#include <numeric>
#include <set>
#include <sys/resource.h>
#include <variant>

namespace markov::commands {

nlohmann::json prediction_report(const adapters::PreparedPrediction & input) {
    using namespace trace_graph;
    using Json = nlohmann::json;
    using Clock = std::chrono::steady_clock;
    const auto & source = input.source.context;
    const auto & observations = source.observations();
    const auto backend = adapters::sglang::cache_backend(source);
    const planning::AttentionGeometry * attention = nullptr;
    for (const auto & description : observations.work_descriptions) {
        for (const auto & part : description.recipe) {
            attention = std::get_if<planning::AttentionGeometry>(&part.operation.geometry);
            if (attention) break;
        }
        if (attention) break;
    }
    double context_tokens = 0;
    for (const auto & frame : observations.compute_frames)
        for (const auto tokens : frame.sequence_tokens) context_tokens = std::max(context_tokens, tokens);
    std::set<std::string> final_phases;
    for (const auto & result : observations.batch_results)
        if (!result.final_sends.empty())
            final_phases.insert(observations.compute_frames[result.frame].phase == planning::Phase::Prefill ? "prefill" : "decode");
    Json sampling_context{
        { "max_context_tokens",context_tokens                               },
        {       "final_phases",        final_phases },
        {           "geometry",
         { { "layers", backend.layers },
         { "kv_heads", attention->kv_heads },
         { "key_dim", attention->query_key_dim },
         { "value_dim", attention->value_dim } }   }
    };
    cost::Queries costs(input.costs.models, &source);
    auto cells = Json::array();
    for (const auto & target : input.targets) {
        const Json capacity{
            { "device", target.cache.device_pages },
            {   "host",   target.cache.host_pages }
        };
        if (target.infeasible) {
            cells.push_back({
                {                  "name",           target.name },
                {         "configuration",  target.configuration },
                {  "cache_capacity_pages",              capacity },
                {                "status", "resource_infeasible" },
                {                 "issue",    *target.infeasible },
                {             "cost_gaps",         Json::array() },
                {         "e2e_predicted",                 false },
                {       "formal_http_sec",               nullptr },
                { "background_finish_sec",               nullptr },
                {       "generated_nodes",                     0 },
                {        "executed_nodes",                     0 },
                {       "generated_edges",                     0 },
                {              "requests",         Json::array() },
                {              "coverage",        Json::object() },
                {            "node_scale",         Json::array() },
                {    "backup_submissions",                     0 },
                {      "load_submissions",                     0 },
                {        "simulation_sec",                     0 }
            });
            continue;
        }
        const auto begin = Clock::now();
        planning::CacheProgram program(source,
                                       target.page_tokens,
                                       { target.cache.device_pages, target.cache.host_pages, backend.layers, target.cache.backup_after_hits },
                                       backend,
                                       target.cache.storage,
                                       input.initial,
                                       planning::RegionBackend{ "sglang.ascend.eager", target.result_implementation, target.maximum_prefill_tokens });
        simulation::ProgramResult result;
        if (!program.issue()) result = simulation::ProgramExecution(program, costs, program.resources(), input.node_scale, input.record_graph).run();
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
            if (result.graph) {
                requests.back()["start_node"] = result.graph->operation_nodes[*request.start].value();
                requests.back()["response_node"] = result.graph->operation_nodes[*request.response].value();
            }
            if (const auto * storage = program.storage()) {
                const auto & prefetch = storage->results()[id];
                requests.back()["prefetch"] = {
                    { "requested_pages", prefetch.requested_pages },
                    {   "queried_pages",   prefetch.queried_pages },
                    { "published_pages", prefetch.published_pages },
                    {  "accepted_pages",  prefetch.accepted_pages },
                    {     "worker_done",     prefetch.worker_done },
                    {   "stopped_early",   prefetch.stopped_early }
                };
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
            {  "cache_capacity_pages",                                                                                           capacity },
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
            {            "node_scale",                                                                                 std::move(scaling) },
            {    "backup_submissions",                                                                                  program.backups() },
            {      "load_submissions",                                                                                    program.loads() },
            {              "requests",                                                                                std::move(requests) },
            {        "simulation_sec",                                        std::chrono::duration<double>(Clock::now() - begin).count() }
        });
        if (result.graph) cells.back()["execution_graph"] = generated_graph_report(result);
        if (const auto * storage = program.storage()) {
            const auto & stats = storage->stats();
            cells.back()["host_evicted_pages"] = program.host_evicted_pages();
            cells.back()["storage"] = {
                {         "queries",stats.queries                                    },
                {      "prefetches",      stats.prefetches },
                {      "read_pages",      stats.read_pages },
                {    "copied_pages",    stats.copied_pages },
                { "rejected_copies", stats.rejected_copies },
                {  "write_enqueues",          stats.writes },
                {       "new_pages",       stats.new_pages },
                {  "existing_pages",  stats.existing_pages },
                {            "idle",       storage->idle() },
                {   "qualification",
                 "Buffered file worker costs; query/admission remain logical. Best effort stops at admission; timeout is the earliest deadline, not measured "
                 "scheduler polling."                     }
            };
        }
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
        { "sampling_context", std::move(sampling_context) },
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
            "First-phase initialization remains an estimate. New input preparations use the adapter's specialization rules, not raw token lengths.",
            "Modeled parent waits use target readiness ports. Other source frontend/control intervals retain their opaque work and waits; unidentified "
            "probe overhead and changed control behavior remain uncertain.",
            "Cache query CPU retains the source residual approximation; state-dependent tree overhead and competing I/O/compute need further validation.",
            "HTTP send and post-send CPU are separate; unconsumed overlapped computation and background I/O still drain after HTTP.",
            "No normal E2E accuracy or complete device-causality qualification is implied by execution." } }
    };
}

} // namespace markov::commands
