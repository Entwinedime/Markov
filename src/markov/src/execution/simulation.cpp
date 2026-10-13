#include "markov/execution/simulation.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace markov {
namespace {

using Json = nlohmann::json;

std::string_view connection_rule_name(TemplateConnectionRule rule) {
    switch (rule) {
    case TemplateConnectionRule::ResourcePredecessor:
        return "resource_predecessor";
    case TemplateConnectionRule::InitialResource:
        return "initial_resource";
    case TemplateConnectionRule::ResourceExport:
        return "resource_export";
    case TemplateConnectionRule::Unresolved:
        return "unresolved";
    case TemplateConnectionRule::NativeBoundary:
        return "native_boundary";
    case TemplateConnectionRule::CpuThreadProjection:
        return "cpu_thread_projection";
    }
    std::unreachable();
}

} // namespace

SimulationRuntime::SimulationRuntime(std::span<const Request> requests, Controller & controller, const TemplateBank & templates, const NodeCosts & costs,
                                     std::optional<std::filesystem::path> graph_path)
    : requests_(requests),
      controller_(controller),
      templates_(templates),
      costs_(costs),
      workspace_(templates.workspace_pools()),
      graph_path_(std::move(graph_path)),
      request_timings_(requests.size()) {}

bool SimulationRuntime::LaterEvent::operator()(const QueuedEvent & a, const QueuedEvent & b) const {
    if (a.event.time_ns != b.event.time_ns) return a.event.time_ns > b.event.time_ns;
    return a.sequence > b.sequence;
}

void SimulationRuntime::enqueue(ControlEvent event) { events_.push({ std::move(event), next_event_sequence_++ }); }

std::optional<nlohmann::json> SimulationRuntime::accept(std::span<const ControlAction> actions) {
    std::vector<InvokeStep> invocations;
    for (const auto & action : actions) {
        if (const auto * gap = std::get_if<UnsupportedControl>(&action))
            return report("unsupported",
                          Json::array({
                              { { "kind", "control" }, { "reason", gap->reason }, { "time_ns", time_ns_ } }
            }));

        if (const auto * completion = std::get_if<CompleteRequest>(&action)) {
            request_timings_[completion->request_id].completed_ns = work_.at(completion->published.work_id).completed_ports.at(PortKind::Published);
            continue;
        }

        const auto * invocation = std::get_if<InvokeStep>(&action);
        if (!invocation)
            return report("unsupported",
                          Json::array({
                              { { "kind", "implementation" }, { "reason", "controller_timer_actions_not_implemented" } }
            }));
        invocations.push_back(*invocation);
    }
    if (invocations.empty()) return std::nullopt;

    auto batch = templates_.instantiate(invocations, request_capacity_);
    if (!batch) {
        const auto & gap = batch.error();
        const auto invocation = std::find_if(invocations.begin(), invocations.end(), [&gap](const auto & item) { return item.step == gap.step; });
        const bool missing_observation = gap.kind == TemplateGapKind::Observation;
        auto requests = Json::array();
        for (const auto & request : invocation->input.requests) {
            requests.push_back({
                {               "id", requests_[request.request_id].id },
                {     "input_tokens",             request.input_tokens },
                {   "context_tokens",           request.context_tokens },
                { "generated_tokens",         request.generated_tokens },
                {         "finishes",                 request.finishes },
                {        "pool_slot",                request.pool_slot },
                {         "kv_pages",                 request.kv_pages }
            });
        }

        auto filters = Json::array();
        for (const auto & filter : invocation->input.filters) filters.push_back(filter.describe());

        return report(
            missing_observation ? "needs_data" : "unsupported",
            Json::array({
                { { "kind", missing_observation ? "observation" : "implementation" },
                 { "reason", gap.reason },
                 { "step", ServingContract{}.block(gap.step).name },
                 { "phase", ServingContract{}.phase_name(gap.phase) },
                 { "rank", gap.rank },
                 { "requests", requests },
                 { "batch_filters", filters },
                 { "batch_transition", invocation->input.transition.describe() },
                 { "admission_entry", invocation->input.admission.describe() },
                 { "admission_work",
                    invocation->input.admission_work.describe(invocation->input.admission,
                                                              invocation->input.phase == BatchPhase::Prefill ? invocation->input.requests.size() : 0) },
                 { "batch_merge", invocation->input.merge ? invocation->input.merge->describe() : Json(nullptr) },
                 { "time_ns", time_ns_ } }
        }));
    }

    std::vector<Work> staged;
    for (std::size_t index = 0; index < invocations.size(); ++index) staged.push_back({ invocations[index], std::move((*batch)[index]), time_ns_, {} });

    std::vector<TemplateInstance *> instances;
    for (auto & work : staged)
        for (auto & instance : work.instances) instances.push_back(&instance);
    auto workspace = workspace_;
    for (auto & work : staged) {
        for (auto & instance : work.instances) {
            const auto advanced = workspace.advance(instance, templates_, work.invocation.input);
            if (!advanced)
                return report("unsupported",
                              Json::array({
                                  { { "kind", "structure" }, { "reason", advanced.error() }, { "time_ns", time_ns_ } }
                }));
        }
    }
    const auto connected = assembly_.connect(instances);
    if (!connected)
        return report("unsupported",
                      Json::array({
                          { { "kind", "structure" }, { "reason", connected.error() }, { "time_ns", time_ns_ } }
        }));
    if (!execution_graph_.append(instances, invocations, time_ns_)) {
        return report("unsupported",
                      Json::array({
                          { { "kind", "structure" }, { "reason", "target_dependency_cycle" }, { "time_ns", time_ns_ } }
        }));
    }

    // Commit once after the whole batch has a valid graph. The current
    // non-overlap controller cannot admit the next model batch before completion.
    workspace_ = std::move(workspace);
    for (auto & work : staged) {
        const auto id = work.invocation.work_id;
        if (!work_.emplace(id, std::move(work)).second) throw std::logic_error("duplicate dynamic work identity");
    }
    if (invocations.front().input.phase != BatchPhase::Idle) ++batches_;

    // Work owns the immutable invocation; the cost query borrows it only for
    // this scheduling pass, after staged work has reached its stable owner.
    std::map<WorkId, const InvokeStep *> cost_contexts;
    for (const auto & [id, work] : work_) cost_contexts.emplace(id, &work.invocation);
    auto completions = execution_graph_.schedule(costs_, cost_contexts);
    if (!completions) {
        const auto & gap = completions.error();
        Json description{
            {            "kind",                                 gap.source_span ? "input" : "observation" },
            {          "reason",                                                                gap.reason },
            {         "work_id",                                                                  gap.work },
            {            "rank",                                                                  gap.rank },
            {     "source_span",                  gap.source_span ? Json(*gap.source_span) : Json(nullptr) },
            { "source_manifest", gap.source_manifest ? Json(gap.source_manifest->string()) : Json(nullptr) },
            {         "time_ns",                                                                  time_ns_ }
        };
        if (!gap.service_work.is_null()) description["service_work"] = gap.service_work;
        return report("needs_data", Json::array({ std::move(description) }));
    }
    for (auto & event : *completions) enqueue(std::move(event));
    return std::nullopt;
}

nlohmann::json SimulationRuntime::run(ServingLimits limits) {
    request_capacity_ = limits.request_capacity;
    controller_.initialize(requests_, limits);
    for (RequestId id = 0; id < requests_.size(); ++id) enqueue({ requests_[id].arrival_time_ns, RequestArrived{ id } });

    while (!events_.empty()) {
        time_ns_ = events_.top().event.time_ns;
        std::vector<ControlEvent> simultaneous;
        while (!events_.empty() && events_.top().event.time_ns == time_ns_) {
            simultaneous.push_back(events_.top().event);
            events_.pop();
        }

        // Apply all physical completions before a controller sees this instant.
        // Arrivals at the same instant participate in that single decision.
        for (const auto & event : simultaneous) {
            const auto * completion = std::get_if<WorkPortCompleted>(&event.payload);
            if (!completion) continue;
            auto & work = work_.at(completion->port.work_id);
            work.completed_ports.emplace(completion->port.port, time_ns_);
            if (completion->port.port == PortKind::Published) {
                for (const auto & request : work.invocation.input.requests) {
                    auto & timing = request_timings_[request.request_id];
                    if (!timing.first_published_ns) timing.first_published_ns = time_ns_;
                }
            }
        }

        std::vector<WorkId> in_flight;
        for (const auto & [id, work] : work_) {
            if (work.completed_ports.size() < work.instances.front().definition->ports.size()) in_flight.push_back(id);
        }

        const auto actions = controller_.handle(simultaneous, RuntimeView{ time_ns_, in_flight });
        if (auto stopped = accept(actions)) return std::move(*stopped);
    }

    if (controller_.finish().completed_requests != requests_.size()) {
        return report("unsupported",
                      Json::array({
                          { { "kind", "control" }, { "reason", "remaining_requests_cannot_be_admitted" }, { "time_ns", time_ns_ } }
        }));
    }

    return report("complete", Json::array());
}

nlohmann::json SimulationRuntime::report(std::string_view status, const nlohmann::json & gaps) const {
    const ServingContract serving;
    auto graph = Json::array();
    auto connection_rules = Json::object();

    // Cuts depend only on the immutable source definition. Compute them once
    // per report, rather than once for every generated target batch.
    std::map<const TemplateDefinition *, Json> source_cuts;

    for (const auto & [id, work] : work_) {
        for (const auto & instance : work.instances) {
            for (const auto & target : instance.connections) {
                const std::string name(connection_rule_name(target.binding.rule));
                connection_rules[name] = connection_rules.value(name, std::size_t{ 0 }) + 1;
            }
        }
        if (!graph_path_) continue;

        auto dependencies = Json::array();
        for (const auto & dependency : work.invocation.dependencies) {
            dependencies.push_back({
                { "work_id",                 dependency.work_id },
                {    "port", serving.port_name(dependency.port) }
            });
        }

        auto port_dependencies = Json::array();
        for (const auto & dependency : work.invocation.port_dependencies) {
            port_dependencies.push_back({
                {     "work_id",                 dependency.predecessor.work_id },
                {        "port", serving.port_name(dependency.predecessor.port) },
                { "target_port",           serving.port_name(dependency.target) }
            });
        }

        auto instances = Json::array();
        for (const auto & instance : work.instances) {
            const auto & definition = *instance.definition;
            auto [cuts, first_use] = source_cuts.try_emplace(&definition, Json::object());
            std::size_t services = 0;
            std::size_t waits = 0;
            std::size_t annotations = 0;
            for (const auto & node : definition.graph) {
                switch (node.role()) {
                case TemplateNodeRole::Service:
                    ++services;
                    break;
                case TemplateNodeRole::Wait:
                    ++waits;
                    break;
                case TemplateNodeRole::Annotation:
                    ++annotations;
                    break;
                }
            }

            auto local_ports = Json::object();
            for (const auto & [port, boundary] : definition.ports) local_ports[std::string(serving.port_name(port))] = boundary;

            Json layout = nullptr;
            if (definition.input_layout) {
                layout = {
                    {  "shape",  definition.input_layout->shape },
                    { "stride", definition.input_layout->stride },
                    {  "dtype",  definition.input_layout->dtype },
                    { "device", definition.input_layout->device }
                };
            }

            auto connections = Json::array();
            for (const auto & target : instance.connections) {
                const auto & connection = target.connection;
                const auto & binding = target.binding;
                auto peers = Json::array();
                for (const auto & endpoint : binding.peers) {
                    peers.push_back({
                        {  "work_id",  endpoint.work_id },
                        {     "rank",     endpoint.rank },
                        { "boundary", endpoint.boundary }
                    });
                }
                const auto rule = connection_rule_name(binding.rule);

                connections.push_back({
                    {           "binding_rule",                                                                                    rule },
                    {            "source_edge",                                                                  connection.source_edge },
                    {              "direction", connection.direction == TemplateConnectionDirection::Incoming ? "incoming" : "outgoing" },
                    {         "local_boundary",                                                               connection.local_boundary },
                    { "remote_source_boundary",                                                       connection.remote_source_boundary },
                    {    "remote_source_calls",                                                          connection.remote_source_calls },
                    {           "target_peers",                                                                        std::move(peers) }
                });
            }

            std::size_t parameterized_kernels = 0;
            for (const auto & node : definition.graph) {
                const auto & node_observation = definition.observation(node);
                if (!node_observation.kernel) continue;
                if (definition.model_kernel_layouts.contains(node_observation.source_span)
                    || definition.attention_kernel_layouts.contains(node_observation.source_span)
                    || instance.kernel_overrides.contains(node_observation.source_span))
                    ++parameterized_kernels;
            }

            auto kernels = Json::array();
            for (const auto & node : definition.graph) {
                const auto & node_observation = definition.observation(node);
                if (!node_observation.kernel) continue;
                auto kernel = node_observation.kernel->describe();
                kernel["source_span"] = node_observation.source_span;
                kernels.push_back(std::move(kernel));
            }

            auto transfers = Json::array();
            for (const auto & node : definition.graph) {
                const auto & node_observation = definition.observation(node);
                if (!node_observation.transfer) continue;
                auto record = node_observation.transfer->describe();
                record["source_span"] = node_observation.source_span;
                transfers.push_back(std::move(record));
            }

            auto transformed_transfers = Json::array();
            for (const auto & [span, transfer] : instance.transfer_overrides) {
                auto record = transfer.describe();
                record["source_span"] = span;
                transformed_transfers.push_back(std::move(record));
            }

            auto transformed_kernels = Json::array();
            for (const auto & [span, kernel] : instance.kernel_overrides) {
                auto record = kernel.describe();
                record["source_span"] = span;
                transformed_kernels.push_back(std::move(record));
            }

            auto model_layouts = Json::array();
            std::size_t partial_operand_layouts = 0;
            for (const auto & [span, layout] : definition.model_kernel_layouts) {
                auto record = layout.annotations;
                record["source_span"] = span;
                model_layouts.push_back(std::move(record));
                if (definition.graph.model_rule(layout.node).has_partial_operands()) ++partial_operand_layouts;
            }

            auto attention_layouts = Json::array();
            for (const auto & [span, layout] : definition.attention_kernel_layouts) {
                auto record = layout.annotations;
                record["source_span"] = span;
                attention_layouts.push_back(std::move(record));
            }

            auto rotary_dependencies = Json::array();
            // Multiple producer edges jointly satisfy the pair, without naming
            // either Slice as sin or cos from its observed ordering.
            for (const auto & dependency : definition.rotary_position_dependencies) {
                rotary_dependencies.push_back({
                    {        "producer_span", definition.observation(definition.graph[dependency.producer]).source_span },
                    {        "consumer_span", definition.observation(definition.graph[dependency.consumer]).source_span },
                    {       "required_value",                                                            "sin_cos_pair" },
                    { "consumer_input_slots",                                                                  { 1, 2 } },
                    {                "basis",                                           "source_bound_rotary_embedding" }
                });
            }

            auto source_filters = Json::array();
            for (const auto & filter : definition.filters) source_filters.push_back(filter.describe());

            auto & regions = cuts->second["semantic_regions"];
            if (first_use) {
                regions = Json::array();
                for (const auto & region : definition.regions) {
                    regions.push_back({
                        {   "source_call",                                        region.source_call },
                        {   "parent_call",                                        region.parent_call },
                        {       "binding",    region.binding ? Json(*region.binding) : Json(nullptr) },
                        {        "module", region.module ? region.module->describe() : Json(nullptr) },
                        {   "local_nodes",                                              region.nodes },
                        {  "local_inputs",                                             region.inputs },
                        { "local_outputs",                                            region.outputs }
                    });
                    if (region.request_slot) regions.back()["source_request_slot"] = *region.request_slot;
                    if ((definition.step == StepKind::Schedule || definition.step == StepKind::Prepare || definition.step == StepKind::Publish)
                        && region.module) {
                        const auto cut = definition.semantic_region(region);
                        regions.back()["subgraph_cut"] = cut.describe();
                        const auto ports = definition.region_ports(cut);
                        if (ports) {
                            auto interfaces = Json::array();
                            for (const auto & port : *ports) interfaces.push_back(port.describe());
                            regions.back()["resource_ports"] = std::move(interfaces);
                        }
                        else regions.back()["resource_ports_gap"] = ports.error();
                    }
                }
            }

            auto collectives = Json::array();
            for (const auto & collective : definition.collectives) {
                auto record = collective.observation.describe();
                record["envelope_node"] = collective.envelope_node;
                record["task_nodes"] = collective.task_nodes;
                record["entry_boundaries"] = collective.entry_boundaries;
                record["exit_boundaries"] = collective.exit_boundaries;
                record["internal_dependencies"] = collective.internal_dependencies;
                record["completion_boundary"] = collective.completion ? Json(*collective.completion) : Json(nullptr);
                const auto & count = definition.graph.collective_rule(collective.envelope_node).count;
                const auto scope = definition.observation(definition.graph[collective.envelope_node]).source_scope;
                record["parameter_relation"] = count ? Json{ { "rows", count->rows == CollectiveRows::Tokens ? "input_tokens" : "requests" },
                                                            { "width", count->width },
                                                            { "source_scope", *scope } } : Json(nullptr);
                record["target_logical_count"] = count ? Json(count->count(work.invocation.input)) : Json(nullptr);
                record["physical_transform_qualified"] =
                    !collective.observation.tasks.empty() && instance.communication_overrides.contains(collective.observation.tasks.front().span);
                record["source_scope"] = definition.observation(definition.graph[collective.envelope_node]).source_scope
                                             ? Json(*definition.observation(definition.graph[collective.envelope_node]).source_scope)
                                             : Json(nullptr);
                collectives.push_back(std::move(record));
            }

            auto logits_reshape = Json(nullptr);
            if (const auto index = definition.logits_gather_reshape()) {
                const auto & operation = definition.operations[*index];
                logits_reshape = {
                    {  "source_span",                            operation.host.source_span },
                    { "input_shapes",                               *operation.input_shapes },
                    {  "input_types",                                *operation.input_types },
                    { "device_spans",                                operation.device_spans },
                    {        "basis", "logits_scope_and_B_rank_partitioned_vocabulary_axes" }
                };
            }

            if (const auto service = definition.logits_view_service()) {
                const auto & node = definition.graph[*service];
                logits_reshape["cpu_view_service"] = {
                    { "source_span", definition.observation(node).source_span },
                    {  "local_node",                                 *service },
                    { "local_begin",                               node.begin },
                    {   "local_end",                                 node.end }
                };
            }

            auto & operation_groups = cuts->second["operation_groups"];
            if (first_use) {
                operation_groups = Json::array();
                for (const auto & operation : definition.operations) {
                    auto workers = Json::array();
                    for (const auto & window : operation.workers)
                        workers.push_back({
                            { "source_span", window.source_span },
                            { "local_begin",       window.begin },
                            {   "local_end",         window.end }
                        });
                    // Preparation changes can replace a compute launch with a transfer.
                    // Report the complete CPU/device cut, not just kernel parameters.
                    const bool device_cut = !operation.device_spans.empty()
                                            && (definition.step == StepKind::Prepare || definition.batch_operations.contains(operation.host.source_span));
                    auto replacement_region = Json(nullptr);
                    if (!operation.collective_spans.empty() || device_cut) {
                        const auto region = definition.operation_region(operation);
                        replacement_region = region.describe();
                        if (!operation.collective_spans.empty()) {
                            const auto ports = definition.collective_operation_ports(operation, region);
                            if (ports) replacement_region["collective_ports"] = ports->describe();
                            else replacement_region["collective_ports_gap"] = ports.error();
                        }
                        if (device_cut) {
                            const auto ports = definition.device_operation_ports(operation, region);
                            if (ports) replacement_region["device_ports"] = ports->describe();
                            else replacement_region["device_ports_gap"] = ports.error();
                        }
                    }
                    for (const auto span : operation.device_spans) {
                        const auto layout = definition.model_kernel_layouts.find(span);
                        if (layout == definition.model_kernel_layouts.end()
                            || definition.graph.model_rule(layout->second.node).role() != ModelKernelRole::PositionSlice)
                            continue;
                        const auto region = definition.operation_region(operation);
                        replacement_region = region.describe();
                        const auto removal = definition.plan_position_materialization_removal(operation);
                        if (removal) {
                            auto added_dependencies = Json::array();
                            for (const auto & edge : removal->added_dependencies)
                                added_dependencies.push_back({
                                    { "relation", SourceGraph::relation_name(edge.relation) },
                                    {   "source",                               edge.source },
                                    {   "target",                               edge.target }
                                });
                            replacement_region["single_token_removal"] = {
                                {               "nodes",                                                             removal->nodes },
                                {        "dependencies",                                                      removal->dependencies },
                                {  "added_dependencies",                                              std::move(added_dependencies) },
                                { "rotary_dependencies",                                               removal->rotary_dependencies },
                                {     "rotary_producer", removal->rotary_producer ? Json(*removal->rotary_producer) : Json(nullptr) }
                            };
                        }
                        else { replacement_region["single_token_removal_gap"] = removal.error(); }
                        break;
                    }
                    operation_groups.push_back({
                        {               "name",                                             operation.name                                               },
                        { "replacement_region",                                                                             std::move(replacement_region) },
                        {               "host",
                         { { "source_span", operation.host.source_span }, { "local_begin", operation.host.begin }, { "local_end", operation.host.end } } },
                        {            "workers",                                                                                        std::move(workers) },
                        {   "collective_spans",                                                                                operation.collective_spans },
                        {       "device_spans",                                                                                    operation.device_spans },
                        {              "basis",
                         operation.device_spans.empty() ? "native_cpu_operation_with_optional_task_queue_flows" : "native_async_npu_and_task_queue_flows" }
                    });
                }
            }

            auto stream_waits = Json::array();
            for (const auto & target : instance.stream_waits) {
                const auto & wait = target.wait;
                const auto & binding = target.binding;
                auto peers = Json::array();
                for (const auto & peer : binding.peers) {
                    peers.push_back({
                        {  "work_id",  peer.work_id },
                        {     "rank",     peer.rank },
                        { "boundary", peer.boundary }
                    });
                }
                stream_waits.push_back({
                    {      "source_span",                                                 wait.source_span },
                    {            "scope", wait.scope == SynchronizationScope::Device ? "device" : "stream" },
                    {    "native_stream",   wait.native_stream ? Json(*wait.native_stream) : Json(nullptr) },
                    { "local_completion",                                                  wait.completion },
                    {         "resolved",               binding.rule != TemplateConnectionRule::Unresolved },
                    {     "target_peers",                                                 std::move(peers) }
                });
            }

            auto added_resources = Json::array();
            for (const auto & port : instance.topology.added_resource_ports()) added_resources.push_back(port.describe());

            auto workspace_demands = Json::array();
            for (const auto & use : instance.workspace_uses) {
                const auto & demand = use.demand;
                workspace_demands.push_back({
                    { "source_manifest",          use.definition->source_manifest.string() },
                    {     "source_call",                       use.definition->source_call },
                    {      "source_use",                                    use.source_use },
                    {           "begin",                                         use.begin },
                    {             "end",                                           use.end },
                    {      "work_units", demand ? Json(demand->work_units) : Json(nullptr) },
                    {           "bytes",      demand ? Json(demand->bytes) : Json(nullptr) }
                });
            }

            instances.push_back({
                {         "added_resource_ports",                               added_resources                                                 },
                {          "inserted_lane_count",                                                      instance.topology.inserted_lanes().size() },
                {                         "rank",                                                                                definition.rank },
                {              "source_manifest",                                                            definition.source_manifest.string() },
                {                  "source_call",                                                                         definition.source_call },
                {                 "source_order",                                                                        definition.source_order },
                {                "batch_filters",                                                                                 source_filters },
                {                  "batch_merge",                                definition.merge ? definition.merge->describe() : Json(nullptr) },
                {                 "source_batch",                       definition.source_batch ? Json(*definition.source_batch) : Json(nullptr) },
                {                      "binding",                                                                             definition.binding },
                {           "source_connections",                                                                         std::move(connections) },
                {                    "transform", instance.kernel_overrides.empty() ? "request_identity" : "request_identity_and_native_layouts" },
                {               "source_subtree",                                                                      definition.source_subtree },
                {            "workspace_demands",                                                                   std::move(workspace_demands) },
                {             "publication_work",              instance.publication_work ? instance.publication_work->describe() : Json(nullptr) },
                {        "transformed_transfers",                                                               std::move(transformed_transfers) },
                {           "observed_transfers",                                                                           std::move(transfers) },
                {         "observed_collectives",                                                                                    collectives },
                {             "operation_groups",                                                                               operation_groups },
                {        "logits_gather_reshape",                                                                      std::move(logits_reshape) },
                {             "semantic_regions",                                                                                        regions },
                { "rotary_position_dependencies",                                                                            rotary_dependencies },
                {         "model_kernel_layouts",                                                                                  model_layouts },
                {     "attention_kernel_layouts",                                                                              attention_layouts },
                {    "kernel_parameter_coverage",
                 { { "observed_kernels", kernels.size() },
                 { "model_row_layouts", model_layouts.size() },
                 { "partial_operand_layouts", partial_operand_layouts },
                 { "attention_layouts", attention_layouts.size() },
                 { "parameterized_kernels", parameterized_kernels },
                 { "unmapped_kernels", kernels.size() - parameterized_kernels } }                                                               },
                {                 "stream_waits",                                                                                   stream_waits },
                {            "source_span_count",                                                                        definition.graph.size() },
                {   "source_internal_edge_count",                                                         definition.graph.dependencies().size() },
                {         "local_boundary_count",                                                              definition.graph.boundary_count() },
                {             "local_lane_count",                                                                        definition.lanes.size() },
                {            "source_node_roles",                  { { "service", services }, { "wait", waits }, { "annotation", annotations } } },
                {                  "local_ports",                                                                                    local_ports },
                {  "source_input_boundary_count",                                                                       definition.inputs.size() },
                { "source_output_boundary_count",                                                                      definition.outputs.size() },
                {                 "input_layout",                                                                                         layout },
                {          "transformed_kernels",                                                                            transformed_kernels },
                {             "observed_kernels",                                                                                        kernels }
            });
        }

        auto ports = Json::object();
        for (const auto & [port, time] : work.completed_ports) ports[std::string(serving.port_name(port))] = time;

        auto requests = Json::array();
        for (const auto & request : work.invocation.input.requests) {
            requests.push_back({
                {               "id", requests_[request.request_id].id },
                {     "input_tokens",             request.input_tokens },
                {   "context_tokens",           request.context_tokens },
                { "generated_tokens",         request.generated_tokens },
                {         "finishes",                 request.finishes },
                {        "pool_slot",                request.pool_slot },
                {         "kv_pages",                 request.kv_pages }
            });
        }

        auto filters = Json::array();
        for (const auto & filter : work.invocation.input.filters) filters.push_back(filter.describe());

        graph.push_back({
            { "port_dependencies", port_dependencies },
            { "work_id", id },
            { "step", serving.block(work.invocation.step).name },
            { "requests", requests },
            { "free_kv_pages", work.invocation.input.free_kv_pages },
            { "batch_filters", filters },
            { "batch_transition", work.invocation.input.transition.describe() },
            { "admission_entry", work.invocation.input.admission.describe() },
            { "admission_work",
             work.invocation.input.admission_work.describe(work.invocation.input.admission,
             work.invocation.input.phase == BatchPhase::Prefill ? work.invocation.input.requests.size() : 0) },
            { "batch_merge", work.invocation.input.merge ? work.invocation.input.merge->describe() : Json(nullptr) },
            { "phase", serving.phase_name(work.invocation.input.phase) },
            { "dependencies", dependencies },
            { "templates", instances },
            { "ports", ports },
            { "release_ns", work.start_ns ? Json(*work.start_ns) : Json(nullptr) }
        });
    }

    Json metrics = nullptr;
    if (status == "complete") {
        const auto start_ns = std::min_element(requests_.begin(), requests_.end(), [](const Request & a, const Request & b) {
                                  return a.arrival_time_ns < b.arrival_time_ns;
                              })->arrival_time_ns;
        const auto window_ns = time_ns_ - start_ns;

        auto requests = Json::array();
        std::uint64_t tokens = 0;
        for (RequestId id = 0; id < requests_.size(); ++id) {
            const auto & request = requests_[id];
            const auto & timing = request_timings_[id];
            const auto first = *timing.first_published_ns;
            const auto last = *timing.completed_ns;
            tokens += request.output_tokens;
            requests.push_back({
                {                 "id",                                                                                                         request.id },
                { "first_published_ns",                                                                                                              first },
                {       "completed_ns",                                                                                                               last },
                {            "ttft_ns",                                                                                    first - request.arrival_time_ns },
                { "request_latency_ns",                                                                                     last - request.arrival_time_ns },
                {            "tpot_ns", request.output_tokens == 1 ? Json(nullptr) : Json(static_cast<double>(last - first) / (request.output_tokens - 1)) }
            });
        }

        metrics = {
            {        "window_time_ns",                                                                            window_ns },
            { "output_tokens_per_sec", window_ns == 0 ? Json(nullptr) : Json(static_cast<double>(tokens) * 1e9 / window_ns) },
            {              "requests",                                                                             requests }
        };
    }

    Json result = {
        {            "status",                                                                      status                              },
        {             "scope",                                                                                                 "modeled" },
        {           "metrics",                                                                                                   metrics },
        {              "gaps",                                                                                                      gaps },
        {      "timing_basis",             costs_.has_models() ? "empirical_candidates_with_conditional_inputs" : "given_node_durations" },
        {       "assumptions",
         { "explicit givens and/or empirical candidates; conditional execution, not a qualified performance prediction",
         "waits follow dependencies; annotations carry no service charge",
         "initial modeled resources contain no in-flight work",
         "HTTP, tokenizer, initialization, warmup and idle polling are outside scope" }                                                 },
        { "execution_summary", { { "work_nodes", work_.size() }, { "batch_invocations", batches_ }, { "simulated_until_ns", time_ns_ } } }
    };

    result["structure"] = {
        {           "status",                                    status },
        {    "qualification", "pending_source_and_transform_validation" },
        { "connection_rules",               std::move(connection_rules) }
    };

    result["structure"]["graph"] = execution_graph_.describe();
    auto workspace = Json::array();
    for (const auto & [resource, capacity] : workspace_.capacities())
        workspace.push_back({
            {       "resource", resource.describe() },
            { "capacity_bytes",            capacity }
        });
    result["structure"]["workspace_capacities"] = std::move(workspace);

    if (graph_path_) {
        std::filesystem::create_directories(graph_path_->parent_path());
        execution_graph_.write(*graph_path_);
        result["structure"]["graph"]["artifact"] = graph_path_->string();
        // Keep large per-work diagnostics out of the orchestration result.
        // Publish the sidecar atomically before exposing its reference.
        auto details_path = *graph_path_;
        details_path.replace_extension(".details.json");
        auto temporary = details_path;
        temporary += ".tmp";
        std::ofstream stream(temporary);
        stream.exceptions(std::ios::failbit | std::ios::badbit);
        stream << graph << '\n';
        stream.close();
        std::filesystem::rename(temporary, details_path);
        result["execution_details"] = {
            {   "artifact", details_path.string() },
            { "work_count",          work_.size() }
        };
    }

    return result;
}

} // namespace markov
