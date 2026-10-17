#include "markov/execution/execution_details.hpp"

#include <fstream>
#include <utility>

namespace markov {
namespace {
using Json = nlohmann::json;
} // namespace

ExecutionDetails::ExecutionDetails(std::span<const Request> requests) : requests_(requests) {}

std::string_view ExecutionDetails::connection_rule_name(TemplateConnectionRule rule) {
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


std::size_t ExecutionDetails::source(const TemplateDefinition & definition) {
    const auto [found, inserted] = source_indices_.try_emplace(&definition, sources_.size());
    if (!inserted) return found->second;

    const ServingContract serving;
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

    const Json layout = definition.input_layout ? definition.input_layout->describe() : Json(nullptr);


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


    auto regions = Json::array();
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
        if ((definition.step == StepKind::Schedule || definition.step == StepKind::Prepare || definition.step == StepKind::Publish) && region.module) {
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


    auto operation_groups = Json::array();
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
        const bool device_cut =
            !operation.device_spans.empty() && (definition.step == StepKind::Prepare || definition.batch_operations.contains(operation.host.source_span));
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
            if (layout == definition.model_kernel_layouts.end() || definition.graph.model_rule(layout->second.node).role() != ModelKernelRole::PositionSlice)
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
            {               "name",                                                                                                                   operation.name },
            { "replacement_region",                                                                                                    std::move(replacement_region) },
            {               "host",  { { "source_span", operation.host.source_span }, { "local_begin", operation.host.begin }, { "local_end", operation.host.end } } },
            {            "workers",                                                                                                               std::move(workers) },
            {   "collective_spans",                                                                                                       operation.collective_spans },
            {       "device_spans",                                                                                                           operation.device_spans },
            {              "basis", operation.device_spans.empty() ? "native_cpu_operation_with_optional_task_queue_flows" : "native_async_npu_and_task_queue_flows" }
        });
    }
    sources_.push_back({
        {                         "rank",                      definition.rank                                         },
        {              "source_manifest",                                           definition.source_manifest.string() },
        {                  "source_call",                                                        definition.source_call },
        {                 "source_order",                                                       definition.source_order },
        {                "batch_filters",                                                     std::move(source_filters) },
        {                  "batch_merge",               definition.merge ? definition.merge->describe() : Json(nullptr) },
        {                 "source_batch",      definition.source_batch ? Json(*definition.source_batch) : Json(nullptr) },
        {                      "binding",                                                            definition.binding },
        {               "source_subtree",                                                     definition.source_subtree },
        {           "observed_transfers",                                                          std::move(transfers) },
        {         "observed_collectives",                                                        std::move(collectives) },
        {             "operation_groups",                                                   std::move(operation_groups) },
        {        "logits_gather_reshape",                                                     std::move(logits_reshape) },
        {             "semantic_regions",                                                            std::move(regions) },
        { "rotary_position_dependencies",                                                std::move(rotary_dependencies) },
        {    "kernel_parameter_coverage",
         { { "observed_kernels", kernels.size() },
         { "model_row_layouts", model_layouts.size() },
         { "partial_operand_layouts", partial_operand_layouts },
         { "attention_layouts", attention_layouts.size() } }                                                           },
        {         "model_kernel_layouts",                                                      std::move(model_layouts) },
        {     "attention_kernel_layouts",                                                  std::move(attention_layouts) },
        {            "source_span_count",                                                       definition.graph.size() },
        {   "source_internal_edge_count",                                        definition.graph.dependencies().size() },
        {         "local_boundary_count",                                             definition.graph.boundary_count() },
        {             "local_lane_count",                                                       definition.lanes.size() },
        {            "source_node_roles", { { "service", services }, { "wait", waits }, { "annotation", annotations } } },
        {                  "local_ports",                                                        std::move(local_ports) },
        {  "source_input_boundary_count",                                                      definition.inputs.size() },
        { "source_output_boundary_count",                                                     definition.outputs.size() },
        {                 "input_layout",                                                                        layout },
        {             "observed_kernels",                                                            std::move(kernels) }
    });
    return found->second;
}

void ExecutionDetails::add(const InvokeStep & invocation, std::span<const TemplateInstance> definitions,
                           const std::map<PortKind, std::int64_t> & completed_ports, std::optional<std::int64_t> start_ns) {
    const ServingContract serving;
    auto dependencies = Json::array();
    for (const auto & dependency : invocation.dependencies) {
        dependencies.push_back({
            { "work_id",                 dependency.work_id },
            {    "port", serving.port_name(dependency.port) }
        });
    }

    auto port_dependencies = Json::array();
    for (const auto & dependency : invocation.port_dependencies) {
        port_dependencies.push_back({
            {     "work_id",                 dependency.predecessor.work_id },
            {        "port", serving.port_name(dependency.predecessor.port) },
            { "target_port",           serving.port_name(dependency.target) }
        });
    }

    auto instances = Json::array();
    for (const auto & instance : definitions) {
        const auto & definition = *instance.definition;
        const auto source_index = source(definition);
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

        auto collectives = Json::array();
        for (std::size_t index = 0; index < definition.collectives.size(); ++index) {
            const auto & collective = definition.collectives[index];
            const auto & count = definition.graph.collective_rule(collective.envelope_node).count;
            collectives.push_back({
                {            "source_collective",                                       index                                                 },
                {         "target_logical_count",                                 count ? Json(count->count(invocation.input)) : Json(nullptr) },
                { "physical_transform_qualified",
                 !collective.observation.tasks.empty() && instance.communication_overrides.contains(collective.observation.tasks.front().span) }
            });
        }
        const auto kernel_count = sources_[source_index]["kernel_parameter_coverage"]["observed_kernels"].get<std::size_t>();
        instances.push_back({
            {         "source_definition",                                         source_index                                          },
            {      "added_resource_ports",                                                                     std::move(added_resources) },
            {       "inserted_lane_count",                                                      instance.topology.inserted_lanes().size() },
            {        "source_connections",                                                                         std::move(connections) },
            {                 "transform", instance.kernel_overrides.empty() ? "request_identity" : "request_identity_and_native_layouts" },
            {         "workspace_demands",                                                                   std::move(workspace_demands) },
            {          "publication_work",              instance.publication_work ? instance.publication_work->describe() : Json(nullptr) },
            {     "transformed_transfers",                                                               std::move(transformed_transfers) },
            {               "collectives",                                                                         std::move(collectives) },
            { "kernel_parameter_coverage",
             { { "parameterized_kernels", parameterized_kernels }, { "unmapped_kernels", kernel_count - parameterized_kernels } }        },
            {              "stream_waits",                                                                        std::move(stream_waits) },
            {       "transformed_kernels",                                                                 std::move(transformed_kernels) }
        });
    }

    auto ports = Json::object();
    for (const auto & [port, time] : completed_ports) ports[std::string(serving.port_name(port))] = time;

    auto requests = Json::array();
    for (const auto & request : invocation.input.requests) {
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
    for (const auto & filter : invocation.input.filters) filters.push_back(filter.describe());

    work_.push_back({
        { "port_dependencies", port_dependencies },
        { "work_id", invocation.work_id },
        { "step", serving.block(invocation.step).name },
        { "requests", requests },
        { "free_kv_pages", invocation.input.free_kv_pages },
        { "batch_filters", filters },
        { "batch_transition", invocation.input.transition.describe() },
        { "admission_entry", invocation.input.admission.describe() },
        { "admission_work",
         invocation.input.admission_work.describe(invocation.input.admission,
         invocation.input.phase == BatchPhase::Prefill ? invocation.input.requests.size() : 0) },
        { "batch_merge", invocation.input.merge ? invocation.input.merge->describe() : Json(nullptr) },
        { "phase", serving.phase_name(invocation.input.phase) },
        { "dependencies", dependencies },
        { "templates", instances },
        { "ports", ports },
        { "release_ns", start_ns ? Json(*start_ns) : Json(nullptr) }
    });
}

void ExecutionDetails::write(const std::filesystem::path & path) const {
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream stream(temporary);
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream << "{\"source_definitions\":" << sources_ << ",\"work\":" << work_ << "}\n";
    stream.close();
    std::filesystem::rename(temporary, path);
}

} // namespace markov
