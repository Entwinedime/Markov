#include "markov/commands/source.hpp"

#include "markov/adapters/source.hpp"
#include "markov/commands/costs.hpp"
#include "markov/trace_graph/cost/queries.hpp"
#include "markov/trace_graph/planning/cache.hpp"
#include "markov/trace_graph/planning/cache_program.hpp"
#include "markov/trace_graph/planning/compute_plan.hpp"
#include "markov/trace_graph/planning/compute_program.hpp"
#include "markov/trace_graph/simulation/program_execution.hpp"

#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov::commands {
namespace {

using namespace trace_graph;
using Json = nlohmann::json;

Json cost_gap_json(const simulation::CostGap & gap) {
    auto work = work_json(gap.work);
    work["status"] = cost_status_name(gap.response.status);
    work["affected_features"] = gap.response.affected_features;
    work["model"] = gap.response.model ? Json(*gap.response.model) : Json(nullptr);
    return {
        { "kind",      "cost_gap" },
        { "work", std::move(work) }
    };
}

struct SubsetExecution {
    std::size_t completed_frames = 0, executed_nodes = 0, service_operations = 0, queue_dependencies = 0;
    std::map<std::string, std::size_t> coverage;
    Json issue = nullptr;

    void evaluate(const source::SourceContext & source, const planning::ComputePlan & plan, cost::Queries & costs, source::Index frame) {
        if (!issue.is_null()) return;
        if (!plan.missing_row_bindings.empty()) {
            issue = {
                {              "kind",    "missing_work_binding" },
                {             "frame",                     frame },
                { "source_operations", plan.missing_row_bindings }
            };
            return;
        }

        planning::ComputeProgram program(source, plan, frame);
        const auto unbound = program.unresolved_queues();
        if (!unbound.empty()) {
            issue = {
                {              "kind", "unresolved_resource_order" },
                {             "frame",                       frame },
                { "source_operations",               Json::array() }
            };
            for (const auto operation : unbound) issue["source_operations"].push_back(operation);
            return;
        }
        auto result = simulation::ProgramExecution(program, costs, source.observations().resources.size()).run();
        executed_nodes += result.execution.executed;
        for (const auto & [kind, count] : result.coverage) coverage[cost_coverage_name(kind)] += count;
        if (result.cost_gap) {
            issue = cost_gap_json(*result.cost_gap);
            issue["frame"] = frame;
            return;
        }
        ++completed_frames;
        for (const auto & operation : plan.operations) service_operations += operation.work.has_value();
        queue_dependencies += source.frame_queue_order(frame).size();
    }

    Json report(std::size_t requests, std::size_t evaluations) const {
        return {
            {               "status",issue.is_null() ? "known_subset_executed" : "stopped"                                     },
            {     "completed_frames",                                      completed_frames },
            {       "executed_nodes",                                        executed_nodes },
            {   "service_operations",                                    service_operations },
            {   "queue_dependencies",                                    queue_dependencies },
            {             "coverage",                                              coverage },
            {      "logical_queries",                                              requests },
            { "response_evaluations",                                           evaluations },
            {                "issue",                                                 issue },
            {        "e2e_predicted",                                                 false },
            {        "qualification",
             "Recipe-internal edges and preserved observed stream order only. Placement changes, cross-stream data, cache, CPU and communication dependencies "
             "are incomplete; no frame or E2E "
             "latency is reported."                                                        }
        };
    }
};

Json cache_execution(const source::SourceContext & source, std::size_t page_tokens, const adapters::sglang::CacheSettings & settings,
                     const planning::CacheBackend & backend, cost::Queries & queries) {
    const auto query_begin = queries.requests(), evaluation_begin = queries.evaluations();
    planning::CacheProgram program(source,
                                   page_tokens,
                                   { settings.device_pages, settings.host_pages, backend.layers, settings.backup_after_hits },
                                   backend,
                                   settings.storage);
    auto result = simulation::ProgramExecution(program, queries, program.resources()).run();
    auto requests = Json::array();
    for (std::size_t i = 0; i < program.requests().size(); ++i) {
        const auto & request = program.requests()[i];
        auto item = Json{
            {             "request", source.observations().requests[i].identity },
            {           "completed",                          request.completed },
            { "reused_device_pages",                   request.device_hit_pages },
            {        "loaded_pages",                       request.loaded_pages },
            {      "prefill_tokens",                     request.prefill_tokens }
        };
        if (const auto * storage = program.storage()) {
            const auto & prefetch = storage->results()[i];
            item["prefetch"] = {
                {            "examined",        prefetch.examined },
                {    "controller_ready",           prefetch.ready },
                {         "worker_done",     prefetch.worker_done },
                {     "requested_pages", prefetch.requested_pages },
                {       "queried_pages",   prefetch.queried_pages },
                {     "published_pages", prefetch.published_pages },
                {      "accepted_pages",  prefetch.accepted_pages },
                { "inserted_host_pages",  prefetch.inserted_pages },
                {       "stopped_early",   prefetch.stopped_early },
                {      "rebased_suffix",  prefetch.rebased_suffix }
            };
        }
        requests.push_back(std::move(item));
    }
    Json issue = nullptr;
    if (result.cost_gap) issue = cost_gap_json(*result.cost_gap);
    else if (program.issue())
        issue = {
            { "kind", *program.issue() }
        };
    std::map<std::string, std::size_t> coverage;
    for (const auto & [kind, count] : result.coverage) coverage[cost_coverage_name(kind)] = count;
    Json report = {
        {                     "status",issue.is_null() ? "known_cache_compute_subset_executed" : "stopped"                                       },
        {                      "issue",                                                               std::move(issue) },
        {      "device_capacity_pages",                                                          settings.device_pages },
        {        "host_capacity_pages",                                                            settings.host_pages },
        {          "backup_after_hits", settings.backup_after_hits ? Json(*settings.backup_after_hits) : Json(nullptr) },
        {               "participants",                                                           backend.participants },
        {                     "layers",                                                                 backend.layers },
        { "page_bytes_per_participant",                                          page_tokens * backend.bytes_per_token },
        {          "peak_device_pages",                                                    program.peak_device_pages() },
        {            "peak_host_pages",                                                      program.peak_host_pages() },
        {         "backup_submissions",                                                              program.backups() },
        {           "load_submissions",                                                                program.loads() },
        {           "device_evictions",                                                     program.device_evictions() },
        {             "host_evictions",                                                       program.host_evictions() },
        {                   "requests",                                                            std::move(requests) },
        {             "executed_nodes",                                                      result.execution.executed },
        {                   "coverage",                                                            std::move(coverage) },
        {            "logical_queries",                                               queries.requests() - query_begin },
        {       "response_evaluations",                                       queries.evaluations() - evaluation_begin },
        {              "e2e_predicted",                                                                          false },
        {              "qualification",
         "Cache I/O and the known compute subset only. Symmetric participant services and dedicated copy queues are assumed. Query/admission and other CPU, "
         "communication and complete cross-stream dependencies remain omitted; no HTTP latency is reported."          }
    };
    if (const auto * storage = program.storage()) {
        const auto & stats = storage->stats();
        const auto & config = *settings.storage;
        const std::map<planning::PrefetchPolicy, std::string> policies{
            {   planning::PrefetchPolicy::Complete, "wait_complete" },
            { planning::PrefetchPolicy::BestEffort,   "best_effort" },
            {    planning::PrefetchPolicy::Timeout,       "timeout" }
        };
        report["storage"] = {
            {        "initial_state",                         "empty"                                     },
            {       "capacity_pages", config.capacity_pages ? Json(*config.capacity_pages) : Json(nullptr) },
            { "prefetch_stop_policy",                                           policies.at(config.policy) },
            {              "queries",                                                        stats.queries },
            {           "prefetches",                                                     stats.prefetches },
            {           "read_pages",                                                     stats.read_pages },
            {         "copied_pages",                                                   stats.copied_pages },
            {      "rejected_copies",                                                stats.rejected_copies },
            {       "write_enqueues",                                                         stats.writes },
            {            "new_pages",                                                      stats.new_pages },
            {       "existing_pages",                                                 stats.existing_pages },
            {       "disk_evictions",                                                 stats.disk_evictions },
            {  "peak_reserved_pages",                                            stats.peak_reserved_pages },
            {    "peak_stored_pages",                                              stats.peak_stored_pages },
            {                 "idle",                                                      storage->idle() },
            {        "qualification",
             "Native service order with inferred stage costs. Queries and admission are logical only; best effort therefore stops at immediate admission. "
             "Timeouts use this partial cost set, not complete runtime latency. Existing-key materialization is used to infer the write split; phase "
             "durations are not separately measured."                                                     }
        };
    }
    return report;
}

} // namespace

nlohmann::json page_work_report(const adapters::PreparedSource & source, std::span<const std::size_t> page_sizes_tokens, const adapters::SelectedCosts * costs,
                                const adapters::sglang::CacheSettings * cache) {
    const auto & observations = source.context.observations();
    const auto & paths = observations.token_paths;
    std::optional<cost::Queries> queries;
    if (costs) queries.emplace(costs->models);
    const auto backend = cache ? std::optional(adapters::sglang::cache_backend(source.context)) : std::nullopt;
    auto targets = Json::array();
    for (const auto size : page_sizes_tokens) {
        SubsetExecution execution;
        const auto query_begin = queries ? queries->requests() : 0;
        const auto evaluation_begin = queries ? queries->evaluations() : 0;
        struct WorkCounts {
            std::size_t instances = 0, source_work_unchanged = 0, source_service_bound = 0;
        };
        std::size_t corpus_pages = 0;
        for (const auto & [identity, tokens] : observations.request_tokens) {
            if (tokens.committed) corpus_pages += paths.length(tokens.committed->path) / size;
        }

        // This diagnostic retains the complete corpus as one logical layer.
        // Its capacity follows the input; it is not a target hardware setting.
        planning::Cache prefixes({ corpus_pages, 0, 1, std::nullopt });
        std::map<planning::Work, WorkCounts> projected_work;
        std::size_t missing_projection_inputs = 0, missing_row_bindings = 0, dependencies = 0, views = 0;
        auto requests = Json::array();
        for (const auto & request : observations.requests) {
            const auto found = observations.request_tokens.find(request.identity);
            if (found == observations.request_tokens.end() || !found->second.lookup_input || !found->second.prompt || !found->second.committed)
                throw std::runtime_error("Request token ancestry is incomplete: " + request.identity);

            const auto & tokens = found->second;
            const auto prompt = tokens.prompt->path;
            const auto lookup_input = tokens.lookup_input->path;
            const auto committed = tokens.committed->path;
            if (paths.prefix(prompt, paths.length(lookup_input)) != lookup_input || paths.prefix(committed, paths.length(prompt)) != prompt)
                throw std::runtime_error("Request lookup_input or committed path is not consistent with its prompt: " + request.identity);

            const auto lookup_pages = paths.pages(lookup_input, size);
            const auto committed_pages = paths.pages(committed, size);
            const auto prior_commit = prefixes.match(lookup_pages);
            const auto prior_pages = prefixes.page_count(prior_commit.device);
            prefixes.pin_device(prior_commit.device);

            const auto existing = prefixes.match(committed_pages);
            const auto buffer = prefixes.reserve(planning::Tier::Device, committed_pages.size() - prefixes.page_count(existing.device)).value();
            prefixes.commit_device(committed_pages, buffer);
            prefixes.release(buffer);
            prefixes.unpin_device(prior_commit.device);

            const auto frames = source.context.request_frames(request.identity);
            missing_projection_inputs += frames.empty();
            for (const auto frame_id : frames) {
                const auto & frame = observations.compute_frames[frame_id];
                if (frame.requests.size() != 1 || frame.sequence_tokens.size() != 1 || !frame.query_tokens || frame.query_tokens->size() != 1) {
                    ++missing_projection_inputs;
                    if (queries && execution.issue.is_null())
                        execution.issue = {
                            {  "kind", "unsupported_frame_inputs" },
                            { "frame",                   frame_id }
                        };
                    continue;
                }
                const auto query = frame.phase == planning::Phase::Prefill ? paths.length(prompt) - prior_pages * size : frame.query_tokens->front();
                const auto context = frame.phase == planning::Phase::Prefill ? paths.length(prompt) : frame.sequence_tokens.front();
                const auto plan = planning::project_compute(source.context, frame_id, { query, query, context, 1, static_cast<double>(size) });
                missing_row_bindings += plan.missing_row_bindings.size();
                if (queries && !cache) execution.evaluate(source.context, plan, *queries, frame_id);
                dependencies += plan.dependencies.size();
                for (const auto & operation : plan.operations) {
                    if (!operation.work) {
                        ++views;
                        continue;
                    }
                    auto & counts = projected_work[plan.work[*operation.work]];
                    ++counts.instances;
                    counts.source_work_unchanged += operation.source_work_unchanged;
                    counts.source_service_bound += operation.source_work_unchanged && source.context.device_service(operation.source_operation).available();
                }
            }
            requests.push_back({
                {                        "request",                                  request.identity },
                {                         "formal",                                    request.formal },
                {                  "prompt_tokens",                              paths.length(prompt) },
                {     "lookup_input_page_prefixes",                                      lookup_pages },
                {        "committed_page_prefixes",                                   committed_pages },
                {        "maximum_reusable_tokens",                        lookup_pages.size() * size },
                {  "prior_committed_prefix_tokens",                                prior_pages * size },
                {         "minimum_prefill_tokens", paths.length(prompt) - lookup_pages.size() * size },
                { "uncached_committed_tail_tokens",                    paths.length(committed) % size }
            });
        }
        auto compute = Json::array();
        for (const auto & [work, counts] : projected_work) {
            auto value = work_json(work);
            value["instances"] = counts.instances;
            value["source_work_unchanged"] = counts.source_work_unchanged;
            value["source_service_bound"] = counts.source_service_bound;
            compute.push_back(std::move(value));
        }
        auto segments = Json::array();
        for (std::size_t id = 1; id < prefixes.tree().nodes().size(); ++id) {
            const auto & node = prefixes.tree().nodes()[id];
            segments.push_back({
                {       "node",                id },
                {     "parent",       node.parent },
                { "end_prefix", node.pages.back() },
                {      "pages", node.pages.size() }
            });
        }
        auto cache_result = cache ? cache_execution(source.context, size, *cache, *backend, *queries) : Json(nullptr);
        targets.push_back({
            { "page_size_tokens", size },
            { "requests", std::move(requests) },
            { "corpus_device_pages", prefixes.used_pages(planning::Tier::Device) },
            { "projected_compute_work", std::move(compute) },
            { "projected_compute_dependencies", dependencies },
            { "projected_view_operations", views },
            { "cost_execution",
             queries && !cache ? execution.report(queries->requests() - query_begin, queries->evaluations() - evaluation_begin) : Json(nullptr) },
            { "cache_execution", std::move(cache_result) },
            { "missing_projection_inputs", missing_projection_inputs },
            { "missing_row_bindings", missing_row_bindings },
            { "radix_segments", std::move(segments) }
        });
    }
    auto barriers = Json::array();
    for (const auto & barrier : observations.workload_barriers)
        barriers.push_back({
            { "before_request",  barrier.before_request },
            {        "control",         "drain_hicache" },
            {           "file",     barrier.record.file },
            {         "record", barrier.record.position }
        });

    auto report = Json{
        {                 "status","target_page_workload_compiled"                                   },
        {               "manifest",               source.manifest.path.string() },
        {                  "scope", "request_ancestry_and_target_page_geometry" },
        {                "targets",                          std::move(targets) },
        {      "workload_barriers",                         std::move(barriers) },
        {    "source_preparations",                                           1 },
        {     "source_graph_nodes", source.context.graph().graph.nodes().size() },
        { "logical_token_prefixes",                                paths.size() },
        {       "target_dag_built",                                       false },
        {        "costs_evaluated",         queries && queries->requests() != 0 },
        {          "qualification",
         "Page geometry describes the unbounded corpus. Optional cache execution evaluates finite two-tier behavior and a compute subset; neither is a full "
         "target DAG or E2E prediction."                                       }
    };
    if (costs) {
        report["cost_catalog"] = {
            {                  "deployment",                                                                                                              costs->deployment },
            {             "selected_models",                                                                                                           costs->models.size() },
            {             "logical_queries",                                                                                                            queries->requests() },
            {        "response_evaluations",                                                                                                         queries->evaluations() },
            { "source_deployment_qualified",                                                                                                                          false },
            {               "qualification", "Framework/runtime and work geometry checked; the legacy source manifest does not establish complete model/platform identity." }
        };
    }
    report["source_replay"] = source_report(source);
    return report;
}

} // namespace markov::commands
