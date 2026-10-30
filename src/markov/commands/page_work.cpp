#include "markov/commands/source.hpp"

#include "markov/adapters/source.hpp"
#include "markov/commands/costs.hpp"
#include "markov/trace_graph/cost/queries.hpp"
#include "markov/trace_graph/planning/cache.hpp"
#include "markov/trace_graph/planning/cache_program.hpp"
#include "markov/trace_graph/planning/compute_plan.hpp"
#include "markov/trace_graph/planning/compute_program.hpp"
#include "markov/trace_graph/simulation/program_execution.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov::commands {
namespace {

using namespace trace_graph;
using Json = nlohmann::json;

Json preparation_report(const planning::ComputeStats & stats) {
    auto report = Json::object();
    for (const auto & [role, counts] : stats.preparations)
        report[role] = {
            {  "sequences",  counts.sequences },
            { "operations", counts.operations }
        };
    return report;
}

Json frame_ports_report(const planning::ComputeStats & stats, const simulation::ExecutionResult & execution) {
    auto report = Json::array();
    for (const auto & frame : stats.frames) {
        report.push_back({
            {                        "source_frame",                                                                                         frame.source_frame },
            {         "allocator_view_offset_pages",               frame.allocator_view_offset_pages ? Json(*frame.allocator_view_offset_pages) : Json(nullptr) },
            {                          "entered_ns",                                      frame.entry ? Json(execution.starts_ns[*frame.entry]) : Json(nullptr) },
            {                 "caller_submitted_ns",                         frame.submitted ? Json(execution.completions_ns[*frame.submitted]) : Json(nullptr) },
            {                     "result_ready_ns",                   frame.result_ready ? Json(execution.completions_ns[*frame.result_ready]) : Json(nullptr) },
            {   "modeled_result_callers_started_ns",                      frame.callers_entry ? Json(execution.starts_ns[*frame.callers_entry]) : Json(nullptr) },
            { "modeled_result_callers_completed_ns",         frame.callers_completed ? Json(execution.completions_ns[*frame.callers_completed]) : Json(nullptr) },
            {    "modeled_result_work_completed_ns", frame.result_work_completed ? Json(execution.completions_ns[*frame.result_work_completed]) : Json(nullptr) },
            {                     "modeled_send_ns",                                   frame.sent ? Json(execution.completions_ns[*frame.sent]) : Json(nullptr) },
            {           "modeled_work_completed_ns",                         frame.completed ? Json(execution.completions_ns[*frame.completed]) : Json(nullptr) }
        });
    }
    return report;
}

Json kernel_cache_report(const planning::PreparationCache & cache) {
    return {
        {                   "initial_state", cache.initial() ? Json(*cache.initial() == planning::KernelCacheInitial::Empty ? "empty" : "prepared") : Json(nullptr) },
        {         "planned_specializations",                                                                                                cache.specializations() },
        {             "planned_first_loads",                                                                                                    cache.first_loads() },
        { "planned_runtime_initializations",                                                                                        cache.runtime_initializations() }
    };
}

Json cost_gap_json(const simulation::CostGap & gap, const source::SourceContext & source) {
    Json result{
        {              "kind",                                                     "cost_gap" },
        {            "status",                          cost_status_name(gap.response.status) },
        { "affected_features",                                 gap.response.affected_features },
        {             "model", gap.response.model ? Json(*gap.response.model) : Json(nullptr) }
    };
    if (gap.response.status == cost::Status::UnresolvedSourceWait) result["kind"] = "source_dependency_gap";
    if (gap.response.status == cost::Status::UnmodeledSourceWork) result["kind"] = "source_work_gap";
    if (const auto * work = std::get_if<planning::Work>(&gap.work)) result["work"] = work_json(*work);
    else if (const auto * client = std::get_if<planning::SourceClientDelay>(&gap.work)) {
        result["source_client_delay"] = {
            {  "request",  client->request },
            { "response", client->response }
        };
    }
    else if (const auto * source_gap = std::get_if<planning::SourceRegionGap>(&gap.work)) result["source_region_gap"] = source_gap->interval;
    else {
        const auto id = std::get<planning::SourceCpuGap>(gap.work).interval;
        const auto & region = source.cpu_gaps()[id];
        result["source_cpu_gap"] = {
            {         "interval",                                                        id },
            {         "resource", source.observations().resources[region.resource].identity },
            {         "begin_ns",                                           region.begin_ns },
            {           "end_ns",                                             region.end_ns },
            {   "following_file",                                        region.record.file },
            { "following_record",                                    region.record.position }
        };
    }
    return result;
}

Json cost_gaps_json(const std::vector<simulation::CostGap> & gaps, const source::SourceContext & source) {
    auto items = Json::array();
    for (const auto & gap : gaps) items.push_back(cost_gap_json(gap, source));
    return {
        {  "kind",              "binding_gaps" },
        { "scope", "first_incomplete_mutation" },
        {  "gaps",            std::move(items) }
    };
}

struct SubsetExecution {
    explicit SubsetExecution(std::optional<planning::KernelCacheInitial> initial) : preparations(initial) {}
    planning::PreparationCache preparations;
    std::size_t completed_frames = 0, executed_nodes = 0, service_operations = 0;
    std::map<std::string, std::size_t> coverage;
    Json issue = nullptr;

    planning::ComputeStats compute;

    void evaluate(const source::SourceContext & source, std::vector<planning::FrameCompute> frames, cost::Queries & costs) {
        if (!issue.is_null()) return;
        const auto frame_count = frames.size();
        planning::ExecutionQueues queues;
        planning::ComputeProgram program(source,
                                         std::move(frames),
                                         source.observations().resources.size(),
                                         preparations,
                                         queues,
                                         planning::ComputeScope::Compute);
        if (program.issue()) {
            issue = {
                {   "kind", "compute_structure_unavailable" },
                { "detail",                *program.issue() }
            };
            return;
        }
        auto result = simulation::ProgramExecution(program, costs, program.resources()).run();
        executed_nodes += result.execution.executed;
        for (const auto & [kind, count] : result.coverage) { coverage[cost_coverage_name(kind)] += count; }
        const auto & stats = program.stats();
        compute.add(stats);
        service_operations += stats.compute_services;
        if (!result.cost_gaps.empty()) {
            issue = cost_gaps_json(result.cost_gaps, source);
            return;
        }

        completed_frames += frame_count;
    }

    Json report(std::size_t requests, std::size_t evaluations) const {
        return {
            {                  "status",issue.is_null() ? "known_subset_executed" : "stopped"                                        },
            {        "completed_frames",                                      completed_frames },
            {          "executed_nodes",                                        executed_nodes },
            {      "service_operations",                                    service_operations },
            {           "compute_calls",                                 compute.compute_calls },
            {     "collective_services",                           compute.collective_services },
            {    "participant_arrivals",                          compute.participant_arrivals },
            {     "participant_wait_ns",                           compute.participant_wait_ns },
            { "maximum_arrival_span_ns",                       compute.maximum_arrival_span_ns },
            {            "preparations",                           preparation_report(compute) },
            {            "kernel_cache",                     kernel_cache_report(preparations) },
            {       "native_operations",                             compute.native_operations },
            {         "dispatch_delays",                               compute.dispatch_delays },
            {         "source_cpu_gaps",                               compute.source_cpu_gaps },
            {                "coverage",                                              coverage },
            {         "logical_queries",                                              requests },
            {    "response_evaluations",                                           evaluations },
            {                   "issue",                                                 issue },
            {           "e2e_predicted",                                                 false },
            {           "qualification",
             "Known compute calls, metadata and admitted Prefill/Decode allocation share native chains and blocking collective rendezvous. "
             "Allocation uses its observed device stream and precedes forward inputs. Unsorted pools use int64 Prefill or int32 one-token Decode locations. "
             "Idle synchronization "
             "approximates ready-state service, not wake-up under load. All participants share the latest collective completion. Inter-call CPU uses source "
             "residual approximations for the same caller recipe. "
             "Observed serial int64 token output includes pool relay, asynchronous copy and a completion marker. "
             "This compute-only diagnostic excludes result processing and stateful cache controls; request execution evaluates them separately. "
             "Collective envelope CPU, result processing, request control, placement changes and other cross-stream dependencies remain incomplete; no frame "
             "or E2E latency is "
             "reported."                                                                      }
        };
    }
};

Json cache_execution(const source::SourceContext & source, std::size_t page_tokens, const adapters::sglang::CacheSettings & settings,
                     const planning::CacheBackend & backend, cost::Queries & queries, std::optional<planning::KernelCacheInitial> kernel_cache_initial) {
    const auto query_begin = queries.requests(), evaluation_begin = queries.evaluations();
    planning::CacheProgram program(source,
                                   page_tokens,
                                   { settings.device_pages, settings.host_pages, backend.layers, settings.backup_after_hits },
                                   backend,
                                   settings.storage,
                                   kernel_cache_initial);
    auto result = simulation::ProgramExecution(program, queries, program.resources()).run();
    const auto time = [&](std::optional<graph::NodeId> node, bool start = false) -> Json {
        if (!node || result.execution.states[*node] != simulation::OperationState::Complete) return nullptr;
        return start ? result.execution.starts_ns[*node] : result.execution.completions_ns[*node];
    };
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
        item["device_pages_allocated_since_return"] =
            request.device_pages_allocated_since_return ? Json(*request.device_pages_allocated_since_return) : Json(nullptr);
        item["partial_request_ports"] = {
            { "start_ns", time(request.start) },
            { "arrived_ns", time(request.arrived) },
            { "received_ns", time(request.received) },
            { "response_start_ns", time(request.response, true) },
            { "response_complete_ns", time(request.response) }
        };
        item["prefill_prefix_published_ns"] = time(request.prefill_prefix_published);
        item["kv_published_ns"] = time(request.kv_published);
        item["deferred_kv_pages"] = request.deferred_kv_pages;
        item["released_device_pages"] = request.released_device_pages;
        item["query_index_copies"] = request.query_index_copies;
        item["query_operations"] = request.query_operations;
        item["device_pages_released_ns"] = time(request.pages_released);
        item["modeled_work_completed_ns"] = time(request.work_completed);
        if (source.graph().requests.size() == program.requests().size()) {
            const auto & endpoint = source.graph().requests[i];
            item["source_residuals"] = {
                {       "ingress_ns",    source.graph().graph.nodes()[endpoint.ingress].duration_ns },
                { "response_tail_ns", source.graph().graph.nodes()[endpoint.completion].duration_ns }
            };
        }
        if (const auto * storage = program.storage()) {
            const auto & prefetch = storage->results()[i];
            item["prefetch"] = {
                {            "examined",                                                                prefetch.examined },
                {    "controller_ready",                                                                   prefetch.ready },
                {         "worker_done",                                                             prefetch.worker_done },
                {     "requested_pages",                                                         prefetch.requested_pages },
                {       "queried_pages",                                                           prefetch.queried_pages },
                {     "published_pages",                                                         prefetch.published_pages },
                {      "accepted_pages",                                                          prefetch.accepted_pages },
                {           "issued_ns",                   prefetch.issued_ns ? Json(*prefetch.issued_ns) : Json(nullptr) },
                {      "first_check_ns",         prefetch.first_check_ns ? Json(*prefetch.first_check_ns) : Json(nullptr) },
                {         "accepted_ns",               prefetch.accepted_ns ? Json(*prefetch.accepted_ns) : Json(nullptr) },
                {  "worker_finished_ns", prefetch.worker_finished_ns ? Json(*prefetch.worker_finished_ns) : Json(nullptr) },
                { "inserted_host_pages",                                                          prefetch.inserted_pages },
                {       "stopped_early",                                                           prefetch.stopped_early },
                {      "rebased_suffix",                                                          prefetch.rebased_suffix }
            };
        }
        requests.push_back(std::move(item));
    }
    Json issue = nullptr;
    if (!result.cost_gaps.empty()) issue = cost_gaps_json(result.cost_gaps, source);
    else if (program.issue())
        issue = {
            { "kind", *program.issue() }
        };
    std::map<std::string, std::size_t> coverage;
    for (const auto & [kind, count] : result.coverage) coverage[cost_coverage_name(kind)] = count;
    Json report = {
        {                     "status",        issue.is_null() ? "known_cache_compute_subset_executed" : "stopped"                                       },
        {                      "issue",                                                                                                  std::move(issue) },
        {      "device_capacity_pages",                                                                                             settings.device_pages },
        {        "host_capacity_pages",                                                                                               settings.host_pages },
        {          "backup_after_hits",                                    settings.backup_after_hits ? Json(*settings.backup_after_hits) : Json(nullptr) },
        {               "participants",                                                                                              backend.participants },
        {                     "layers",                                                                                                    backend.layers },
        { "page_bytes_per_participant",                                                                             page_tokens * backend.bytes_per_token },
        {          "peak_device_pages",                                                                                       program.peak_device_pages() },
        {            "peak_host_pages",                                                                                         program.peak_host_pages() },
        {         "backup_submissions",                                                                                                 program.backups() },
        {           "load_submissions",                                                                                                   program.loads() },
        {           "device_evictions",                                                                                        program.device_evictions() },
        {         "host_evicted_pages",                                                                                      program.host_evicted_pages() },
        {                   "requests",                                                                                               std::move(requests) },
        {             "executed_nodes",                                                                                         result.execution.executed },
        {           "subset_finish_ns", result.execution.completions_ns.empty() ? Json(nullptr) : Json(std::ranges::max(result.execution.completions_ns)) },
        {                   "coverage",                                                                                               std::move(coverage) },
        {            "logical_queries",                                                                                  queries.requests() - query_begin },
        {       "response_evaluations",                                                                          queries.evaluations() - evaluation_begin },
        {              "e2e_predicted",                                                                                                             false },
        {              "qualification",
         "Cache I/O, known compute calls, metadata and admitted Prefill/Decode allocation share one clock. Native calls share caller/worker chains "
         "and ordered device producers, preserving the allocation-to-forward stream handoff. Unsorted pools use int64 Prefill or int32 one-token Decode "
         "locations. Idle "
         "synchronization approximates ready-state service, not wake-up under load. Symmetric copy services and dedicated copy queues are assumed. "
         "Client ingress/response intervals are source residual approximations for the same request work and frontend deployment, not CPU services. "
         "Inter-call CPU uses corrected source residual approximations for the same caller recipe; known waits require explicit dependencies. "
         "When selected, paired native CPU models use normal C++ API/thread demand and separately profiled port shares; these shares and isolated "
         "dispatch/residual terms have unvalidated normal-runtime uncertainty. They do not supply the remaining framework control CPU. "
         "Partial request ports exclude query/admission CPU work, collective envelope CPU, other cross-stream dependencies and client pacing. "
         "Observed frame overlap advances the next caller after submission with separate queue frontiers; collectives still fence each segment. "
         "Observed serial int64 token output includes pool relay, asynchronous copy and a completion marker. "
         "An unfinished Prefill publishes its page-aligned prompt after participant results are readable and overlap caller submission is available; "
         "write policy can start background backup before Decode completes. Pure result callers wait for their own copy and scheduler submission, "
         "then gate later caller iterations. Cache insertion clones follow target state at result control points; initial index-value copies and empty "
         "release calls, query splitting, matching/table updates and nonempty release control are included. "
         "Result and eviction backups use independent caller, observed-completion and host-clone responses without adding bare DMA. "
         "Write-back eviction retains its caller turn until the explicit copy join; selection, acknowledgement and remaining outer CPU are incomplete. "
         "Publication joins participant index work and required KV outputs. Result caller return and native completion are separate ports. "
         "The send port precedes modeled caller cleanup. "
         "Observed final sends release the client response without deleting later compute or caller work. Missing send observations retain a completion "
         "fence. "
         "New KV pages remain unreadable until the selected outputs are ready; this is a conservative whole-compute publication boundary. "
         "Caller turns, admission and native queue frontiers persist across requests. Ingress retains a source residual, which may already include queue "
         "delay. "
         "All participants share the latest collective completion, omitting per-rank completion spread. No complete HTTP latency is reported."           }
    };
    const auto & compute = program.compute_stats();
    report["collective_execution"] = {
        {                "services",     compute.collective_services },
        {    "participant_arrivals",    compute.participant_arrivals },
        {     "participant_wait_ns",     compute.participant_wait_ns },
        { "maximum_arrival_span_ns", compute.maximum_arrival_span_ns }
    };
    report["preparation_execution"] = preparation_report(compute);
    report["copy_ports"] = Json::array();
    for (const auto & copy : program.completed_copies()) {
        auto returned = Json::array(), observed = Json::array();
        for (const auto node : copy.caller_returns) returned.push_back(time(node));
        for (const auto node : copy.completion_observations) observed.push_back(time(node));
        report["copy_ports"].push_back({
            {                 "direction",         copy.kind == planning::CopyKind::Backup ? "device_to_host" : "host_to_device" },
            {                     "pages",                                                                            copy.pages },
            { "cache_control_returned_ns",                                                                   std::move(returned) },
            {    "completion_observed_ns",                                                                   std::move(observed) },
            {              "published_ns",                                                                  time(copy.published) },
            {          "completion_basis", copy.caller_returns.empty() ? "device_service" : "modeled_observation_upper_interval" }
        });
    }
    report["kernel_cache"] = kernel_cache_report(program.preparations());
    report["native_call_execution"] = {
        {    "compute_calls",     compute.compute_calls },
        { "compute_services",  compute.compute_services },
        {       "operations", compute.native_operations },
        {  "dispatch_delays",   compute.dispatch_delays },
        {  "source_cpu_gaps",   compute.source_cpu_gaps }
    };
    report["native_call_execution"]["frames"] = frame_ports_report(compute, result.execution);
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
             "Native service order with independent stage costs. This component has no request-enqueue/control region, so checks remain logical. "
             "Timeouts use earliest checks, not measured scheduler polling. Write materialization and batch bookkeeping are charged for all pages; "
             "existing/new native set branches have separate responses."                                  }
        };
    }
    return report;
}

} // namespace

nlohmann::json page_work_report(const adapters::PreparedSource & source, std::span<const std::size_t> page_sizes_tokens, const adapters::SelectedCosts * costs,
                                const adapters::sglang::CacheSettings * cache, std::optional<planning::KernelCacheInitial> kernel_cache_initial) {
    const auto & observations = source.context.observations();
    const auto & paths = observations.token_paths;
    std::optional<cost::Queries> queries;
    if (costs) queries.emplace(costs->models, &source.context, source.cpu_costs ? &source.cpu_costs->intervals : nullptr);
    const auto backend = cache ? std::optional(adapters::sglang::cache_backend(source.context)) : std::nullopt;
    auto targets = Json::array();
    for (const auto size : page_sizes_tokens) {
        SubsetExecution execution(kernel_cache_initial);
        const auto query_begin = queries ? queries->requests() : 0;
        const auto evaluation_begin = queries ? queries->evaluations() : 0;
        struct WorkCounts {
            std::size_t instances = 0, source_work_unchanged = 0, source_service_bound = 0;
        };
        std::size_t corpus_pages = 0;
        for (const auto & [identity, tokens] : observations.request_tokens) {
            if (tokens.committed) corpus_pages += (paths.length(tokens.committed->path) + size - 1) / size;
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
            const auto allocation_offset = prefixes.device_pages_allocated_since_return();
            const auto active_pages = (paths.length(committed) + size - 1) / size;
            const auto buffer = prefixes.reserve(planning::Tier::Device, active_pages - prefixes.page_count(existing.device)).value();
            prefixes.commit_device(committed_pages, buffer);
            prefixes.release(buffer);
            prefixes.unpin_device(prior_commit.device);

            const auto frames = source.context.request_frames(request.identity);
            missing_projection_inputs += frames.empty();
            std::array<std::vector<planning::FrameCompute>, 2> phases;
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
                const planning::FrameInput variables{ query, query, context, 1, static_cast<double>(size), allocation_offset };
                auto plan = planning::project_compute(source.context, frame_id, variables);
                missing_row_bindings += plan.missing_row_bindings.size();
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
                if (queries && !cache) phases[frame.phase == planning::Phase::Prefill ? 0 : 1].push_back({ frame_id, variables, std::move(plan) });
            }
            if (queries && !cache) {
                for (auto & phase : phases)
                    if (!phase.empty()) execution.evaluate(source.context, std::move(phase), *queries);
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
        auto cache_result = cache ? cache_execution(source.context, size, *cache, *backend, *queries, kernel_cache_initial) : Json(nullptr);
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
        auto limitations = Json::array();
        for (const auto id : queries->limited_models()) {
            const auto & model = costs->models.model(id);
            limitations.push_back({
                {          "model",                        id },
                {      "operation",      model.kind.operation },
                { "implementation", model.kind.implementation },
                {   "timing_scope",   model.kind.timing_scope },
                {    "limitations",         model.limitations },
                {        "sources",             model.sources }
            });
        }
        report["cost_catalog"] = {
            {          "queried_model_limitations",                                                                                                         std::move(limitations) },
            {                         "deployment",                                                                                                              costs->deployment },
            {                    "selected_models",                                                                                                           costs->models.size() },
            {                    "logical_queries",                                                                                                            queries->requests() },
            {               "response_evaluations",                                                                                                         queries->evaluations() },
            {         "source_service_evaluations",                                                                                                  queries->source_evaluations() },
            {        "source_residual_evaluations",                                                                                                queries->residual_evaluations() },
            {    "source_cpu_residual_evaluations",                                                                                            queries->cpu_residual_evaluations() },
            {              "source_cpu_gap_domain",                                                                                   cpu_cost_summary(queries->source_cpu_gaps()) },
            {      "source_native_cpu_evaluations",                                                                                                queries->cpu_work_evaluations() },
            {  "source_ready_dispatch_evaluations",                                                                                                queries->dispatch_evaluations() },
            {   "source_ready_control_evaluations",                                                                                                 queries->control_evaluations() },
            { "source_native_cpu_candidate_domain",                                                                              cpu_cost_summary(queries->native_cpu_intervals()) },
            {        "source_deployment_qualified",                                                                                                                          false },
            {                      "qualification", "Framework/runtime and work geometry checked; the legacy source manifest does not establish complete model/platform identity." }
        };
    }
    report["source_replay"] = source_report(source);
    return report;
}

} // namespace markov::commands
