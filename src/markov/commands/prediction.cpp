#include "markov/commands/prediction.hpp"
#include "markov/commands/costs.hpp"
#include "markov/commands/serial_prediction.hpp"
#include "markov/commands/trajectory.hpp"
#include "markov/trace_graph/planning/cache_program.hpp"
#include "markov/trace_graph/simulation/program_execution.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <set>
#include <stdexcept>
#include <sys/resource.h>
#include <variant>

namespace markov::commands {

nlohmann::json prediction_report(const adapters::PreparedPrediction & input) {
    if (std::holds_alternative<std::vector<adapters::SerialPredictionTarget>>(input.targets)) return serial_prediction_report(input);
    using namespace trace_graph;
    using Json = nlohmann::json;
    using Clock = std::chrono::steady_clock;
    const auto & source = input.source.context;
    const auto & observations = source.observations();
    const auto source_backend = adapters::sglang::cache_backend(source);
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
         { { "layers", source_backend.layers },
         { "kv_heads", attention->kv_heads },
         { "key_dim", attention->query_key_dim },
         { "value_dim", attention->value_dim } }   }
    };
    std::map<std::size_t, cost::Queries> queries;
    for (const auto & [tp, selected] : input.costs) queries.try_emplace(tp, selected.models, &source);
    auto cells = Json::array();
    const auto session_begin = Clock::now();
    const auto target_count = std::get<std::vector<adapters::PredictionTarget>>(input.targets).size();
    for (const auto & target : std::get<std::vector<adapters::PredictionTarget>>(input.targets)) {
        if (cells.size() % 32 == 0) {
            const auto predicted = std::ranges::count_if(cells, [](const Json & cell) { return cell.at("status") == "predicted"; });
            std::clog << "prediction " << cells.size() + 1 << '/' << target_count << ", predicted so far: " << predicted
                      << ", elapsed after source preparation: " << std::chrono::duration<double>(Clock::now() - session_begin).count() << " sec" << std::endl;
        }
        const auto & backend = target.backend;
        const auto tp = backend.participants;
        auto & costs = queries.at(tp);
        auto target_sampling = sampling_context;
        target_sampling["geometry"]["kv_heads"] = attention->kv_heads * source_backend.participants / tp;
        const Json capacity{
            { "device", target.cache.device_pages },
            {   "host",   target.cache.host_pages }
        };
        const auto pool_bytes = adapters::sglang::cache_pool_bytes(target.cache, target.page_tokens, backend.bytes_per_token);
        Json reservation{
            { "device_kv_bytes_per_rank", pool_bytes.device_per_rank },
            {   "host_kv_bytes_per_rank",   pool_bytes.host_per_rank }
        };
        if (target.cache.storage && target.cache.storage->capacity_pages) {
            const auto extra = Json::parse(target.configuration.at("hicache_storage_backend_extra_config").get<std::string>());
            reservation["storage_reserved_bytes"] = extra.at("max_size").get<double>() * backend.participants;
        }
        if (target.cache.bandwidth) {
            const auto & rates = *target.cache.bandwidth;
            reservation["bandwidth_bytes_per_sec"] = {
                {  "device_to_host", rates.bytes_per_sec[0] * backend.participants },
                {  "host_to_device", rates.bytes_per_sec[1] * backend.participants },
                { "host_to_storage",                        rates.bytes_per_sec[2] },
                { "storage_to_host",                        rates.bytes_per_sec[3] },
                {  "storage_shared",            rates.storage_shared_bytes_per_sec }
            };
        }
        if (target.infeasible) {
            cells.push_back({
                {                  "name",           target.name },
                {            "deployment",     target.deployment },
                {           "work_layout",    target.work_layout },
                {      "sampling_context",       target_sampling },
                {         "configuration",  target.configuration },
                {  "cache_capacity_pages",              capacity },
                {  "resource_reservation",           reservation },
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
                                       target.regions,
                                       target.cache.bandwidth);
        simulation::ProgramResult result;
        if (!program.issue()) {
            try {
                result = simulation::ProgramExecution(program, costs, program.resources(), input.node_scale, input.record_graph).run();
            }
            catch (const std::runtime_error & error) {
                const auto completed =
                    std::ranges::count_if(program.requests(), [](const planning::CacheRequestResult & request) { return request.completed; });
                throw std::runtime_error("Target " + target.name + ": " + error.what() + "; HTTP completed " + std::to_string(completed) + "/"
                                         + std::to_string(program.requests().size())
                                         + ", storage workers idle=" + (program.storage() && !program.storage()->idle() ? "false" : "true")
                                         + ", issue=" + program.issue().value_or("none"));
            }
        }
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
                {              "request",                 observations.requests[id].identity },
                {               "formal",                   observations.requests[id].formal },
                {             "start_ns",               start ? Json(*start) : Json(nullptr) },
                {          "response_ns",                   end ? Json(*end) : Json(nullptr) },
                {             "http_sec", done ? Json((*end - *start) / 1e9) : Json(nullptr) },
                {     "device_hit_pages",                           request.device_hit_pages },
                {         "loaded_pages",                               request.loaded_pages },
                { "failed_load_attempts",                       request.failed_load_attempts },
                {       "prefill_tokens",                             request.prefill_tokens }
            });
            if (result.graph) {
                requests.back()["start_node"] = result.graph->operation_nodes[*request.start].value();
                requests.back()["response_node"] = result.graph->operation_nodes[*request.response].value();
            }
            if (const auto * storage = program.storage()) {
                const auto & prefetch = storage->results()[id];
                requests.back()["prefetch"] = {
                    {        "poll_rounds",                                                     request.prefetch_poll_rounds },
                    {    "requested_pages",                                                         prefetch.requested_pages },
                    {      "queried_pages",                                                           prefetch.queried_pages },
                    {    "published_pages",                                                         prefetch.published_pages },
                    {     "accepted_pages",                                                          prefetch.accepted_pages },
                    {        "worker_done",                                                             prefetch.worker_done },
                    {      "stopped_early",                                                           prefetch.stopped_early },
                    {          "issued_ns",                   prefetch.issued_ns ? Json(*prefetch.issued_ns) : Json(nullptr) },
                    {     "first_check_ns",         prefetch.first_check_ns ? Json(*prefetch.first_check_ns) : Json(nullptr) },
                    {        "accepted_ns",               prefetch.accepted_ns ? Json(*prefetch.accepted_ns) : Json(nullptr) },
                    { "worker_finished_ns", prefetch.worker_finished_ns ? Json(*prefetch.worker_finished_ns) : Json(nullptr) },
                    {  "query_finished_ns",   prefetch.query_finished_ns ? Json(*prefetch.query_finished_ns) : Json(nullptr) },
                    {  "issue_finished_ns",   prefetch.issue_finished_ns ? Json(*prefetch.issue_finished_ns) : Json(nullptr) },
                    {         "stopped_ns",                 prefetch.stopped_ns ? Json(*prefetch.stopped_ns) : Json(nullptr) }
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
            {            "deployment",                                                                                  target.deployment },
            {           "work_layout",                                                                                 target.work_layout },
            {      "sampling_context",                                                                                    target_sampling },
            {         "configuration",                                                                               target.configuration },
            {  "cache_capacity_pages",                                                                                           capacity },
            {  "resource_reservation",                                                                                        reservation },
            {                "status",
             complete          ? "predicted"
             : program.issue() ? "structure_unavailable"
             : "missing_costs"                                                                                                           },
            {                 "issue",                                           program.issue() ? Json(*program.issue()) : Json(nullptr) },
            {             "cost_gaps",                                                                                    std::move(gaps) },
            {         "e2e_predicted",                                                                                           complete },
            {       "formal_http_sec", complete && formal_begin && formal_end ? Json((*formal_end - *formal_begin) / 1e9) : Json(nullptr) },
            { "background_finish_sec",                                     complete ? Json(result.work_finished_ns / 1e9) : Json(nullptr) },
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
                {                "queries",stats.queries                                           },
                {             "prefetches",             stats.prefetches },
                {        "rank_read_pages",        stats.rank_read_pages },
                {      "rank_copied_pages",      stats.rank_copied_pages },
                {   "rank_rejected_copies",   stats.rank_rejected_copies },
                {         "write_enqueues",                 stats.writes },
                {         "rank_new_pages",         stats.rank_new_pages },
                {    "rank_existing_pages",    stats.rank_existing_pages },
                {    "rank_disk_evictions",    stats.rank_disk_evictions },
                { "peak_rank_stored_pages", stats.peak_rank_stored_pages },
                {                   "idle",              storage->idle() },
                {          "qualification",
                 "Independent rank-local file workers; page counts sum rank services. Enqueue/check are separate; pre-admission control transfers unchanged "
                 "source responses or uses independent target-TP responses. Worker completion retains host ownership until a controller turn. Unidentified "
                 "control retains a source "
                 "residual outside replaced controls. Issue/check/accept responses retain caller ownership; background query groups and idle turns are "
                 "separate. Concurrent file changes remain "
                 "approximate."                                         }
            };
        }
    }
    rusage usage{};
    const auto usage_status = getrusage(RUSAGE_SELF, &usage);
    std::set<std::string> assumptions, sources;
    std::size_t selected_models = 0;
    Json models = Json::object();
    for (const auto & [tp, selected] : input.costs) {
        selected_models += selected.models.size();
        models[std::to_string(tp)] = input.models_paths.at(tp).string();
        for (const auto id : queries.at(tp).limited_models()) {
            const auto & model = selected.models.model(id);
            assumptions.insert(model.limitations.begin(), model.limitations.end());
            sources.insert(model.sources.begin(), model.sources.end());
        }
    }
    return {
        { "deployment", input.deployment },
        { "sampling_context", std::move(sampling_context) },
        { "status", std::ranges::all_of(cells, [](const Json & cell) { return cell.at("e2e_predicted").get<bool>(); }) ? "completed" : "incomplete" },
        { "manifest", input.source.manifest.path.string() },
        { "models", models },
        { "source_prepare_sec", input.source.read_and_interpret_sec + input.source.build_sec },
        { "source_records_read", std::accumulate(input.source.input_record_counts.begin(), input.source.input_record_counts.end(), std::size_t{ 0 }) },
        { "unread_low_level_trace_files", input.source.skipped_files.size() },
        { "source_details_materialized", source.detailed_preparation_sec() != 0 },
        { "process_peak_rss_bytes", usage_status == 0 ? Json(usage.ru_maxrss * 1'024) : Json(nullptr) },
        { "selected_models", selected_models },
        { "model_sources", sources },
        { "cost_assumptions", assumptions },
        { "cells", std::move(cells) },
        { "limitations",
         { "Serial fixed-token eager requests on the declared deployment; target observations are not read.",
            "Host input reads wait for rank-local preparation and prior forward completion; small CPU work before that read is coarsened after readiness. "
            "Independent batch responses estimate wait/spin from metadata host-minus-device intervals, not a strict bound. Host setup, event queueing, "
            "initialization, device tails and contention remain approximate.",
            "Input setup follows rank-local load returns when capacity is ready; GPU prefill retains a conservative whole-load data fence.",
            "Same-TP unchanged CPU broadcasts transfer post-arrival responses; changed-TP frontend responses use independent observations. Local "
            "work/communication overlap remains approximate. Unobserved "
            "control retains its source residual.",
            "CPU and opaque stream responses overlap. Each rank starts at its own ready input; scheduler control rendezvous precedes preparation. Stream "
            "intervals include host starvation and internal communication.",
            "First-phase initialization remains an estimate. New input preparations use the adapter's specialization rules, not raw token lengths.",
            "Modeled parent waits use target readiness ports. Other source frontend/control intervals retain their opaque work and waits; unidentified "
            "probe overhead and changed control behavior remain uncertain.",
            "Background file queries use a shared group-response lane and warm metadata costs; rank-return skew and file changes during a query remain "
            "approximate. File issue/check/accept use closed group responses; other frontend and radix lookup retain source residuals. Competing I/O/compute "
            "needs further validation.",
            "Serial client pacing retains the source interval between requests, outside each request's HTTP. Changed client behavior is not modeled.",
            "HTTP send and post-send CPU are separate; unconsumed overlapped computation and background I/O still drain after HTTP.",
            "No normal E2E accuracy or complete device-causality qualification is implied by execution." } }
    };
}

} // namespace markov::commands
