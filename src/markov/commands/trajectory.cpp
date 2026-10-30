#include "markov/commands/trajectory.hpp"
#include "markov/commands/costs.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace markov::commands {
namespace {

using namespace trace_graph;
using Json = nlohmann::json;

const std::map<graph::NodeKind, std::string> node_kinds{
    {  graph::NodeKind::Service,  "service" },
    { graph::NodeKind::Boundary, "boundary" },
    {    graph::NodeKind::Delay,    "delay" }
};
const std::map<graph::EdgeKind, std::string> edge_kinds{
    {   graph::EdgeKind::ResourceOrder,  "resource_order" },
    {    graph::EdgeKind::ObservedFlow,   "observed_flow" },
    {     graph::EdgeKind::ClientOrder,    "client_order" },
    {      graph::EdgeKind::Submission,      "submission" },
    { graph::EdgeKind::Synchronization, "synchronization" },
    {  graph::EdgeKind::DataDependency, "data_dependency" }
};

std::int64_t nonnegative(const Json & raw) {
    if (!raw.is_number_integer() || raw.get<std::int64_t>() < 0) throw std::runtime_error("Execution graph needs nonnegative integer indices and nanoseconds");
    return raw.get<std::int64_t>();
}

std::string work_name(const Json & node) {
    const auto & work = node.at("work");
    if (work.is_null()) return "";
    return work.at("operation").get<std::string>() + ":" + work.at("implementation").get<std::string>() + ":" + work.at("timing_scope").get<std::string>();
}

} // namespace

nlohmann::json generated_graph_report(const simulation::ProgramResult & result) {
    const auto & snapshot = *result.graph;
    const auto & graph = snapshot.graph;
    auto nodes = Json::array();
    for (const auto & node : graph.nodes())
        nodes.push_back({
            { "duration_ns",         node.duration_ns },
            {    "resource",            node.resource },
            {        "kind", node_kinds.at(node.kind) },
            {        "work",                  nullptr }
        });
    for (std::size_t id = 0; id < snapshot.operation_nodes.size(); ++id) {
        if (!snapshot.operation_nodes[id]) continue;

        auto & node = nodes[*snapshot.operation_nodes[id]];
        const auto & evidence = result.operations[id];
        node["execution_node"] = id;
        node["owner"] = evidence.owner;
        node["start_ns"] = result.execution.starts_ns[id];
        node["completion_ns"] = result.execution.completions_ns[id];
        node["coverage"] = evidence.coverage ? Json(cost_coverage_name(*evidence.coverage)) : Json(nullptr);
        node["action"] = evidence.completion ? Json{ { "kind", evidence.completion->kind }, { "subject", evidence.completion->subject } } : Json(nullptr);
        if (evidence.position) {
            const auto & position = *evidence.position;
            node["position"] = {
                {     "request",         position.request ? Json(*position.request) : Json(nullptr) },
                {   "iteration",                                                 position.iteration },
                { "participant", position.participant ? Json(*position.participant) : Json(nullptr) },
                {        "port",                                         std::string(position.port) }
            };
            if (position.data) node["position"]["data"] = *position.data;
        }
        if (!evidence.work)
            node["origin"] = {
                { "type", "boundary" }
            };
        else if (const auto * work = std::get_if<planning::Work>(&*evidence.work)) {
            node["origin"] = {
                { "type", "work" }
            };
            node["work"] = work_json(*work);
        }
        else if (const auto * gap = std::get_if<planning::SourceRegionGap>(&*evidence.work))
            node["origin"] = {
                {     "type", "source_region" },
                { "interval",   gap->interval }
            };
        else if (const auto * gap = std::get_if<planning::SourceCpuGap>(&*evidence.work))
            node["origin"] = {
                {     "type",  "source_cpu" },
                { "interval", gap->interval }
            };
        else {
            const auto client = std::get<planning::SourceClientDelay>(*evidence.work);
            node["origin"] = {
                {     "type", "source_client" },
                {  "request",  client.request },
                { "response", client.response }
            };
        }
    }
    for (std::size_t id = 0; id < snapshot.timer_nodes.size(); ++id) {
        if (snapshot.timer_nodes[id])
            nodes[*snapshot.timer_nodes[id]]["origin"] = {
                {  "type", "relative_deadline" },
                { "timer",                  id }
            };
    }
    auto edges = Json::array();
    for (const auto & edge : graph.edges()) edges.push_back({ edge.from, edge.to, edge_kinds.at(edge.kind) });
    return {
        { "nodes", std::move(nodes) },
        { "edges", std::move(edges) },
        { "cancelled_operations", std::ranges::count(result.execution.states, simulation::OperationState::Cancelled) },
        { "qualification",
         "Fixed executed path; callback releases, relative deadlines and observed resource order retained. Timing branches are not regenerated." }
    };
}

nlohmann::json trajectory_replay_report(const adapters::PreparedReplay & input) {
    const auto & selected = std::get<adapters::PredictedTrajectory>(input.input);
    const auto & serialized = selected.cell.at("execution_graph");
    const auto & nodes = serialized.at("nodes");
    if (!input.overrides.empty() && input.overrides.rbegin()->first >= nodes.size())
        throw std::runtime_error("Replay override node is outside the recorded graph");
    cost::NodeScale scaling(input.node_scale);
    graph::Graph graph;
    auto inspected = Json::array();
    std::map<std::string, std::size_t> names;
    for (std::size_t id = 0; id < nodes.size(); ++id) {
        const auto & raw = nodes[id];
        const auto name_of_kind = raw.at("kind").get<std::string>();
        const auto kind = std::ranges::find_if(node_kinds, [&](const auto & item) { return item.second == name_of_kind; });
        if (kind == node_kinds.end()) throw std::runtime_error("Unsupported recorded node kind");
        auto duration = nonnegative(raw.at("duration_ns"));
        if (const auto replacement = input.overrides.find(id); replacement != input.overrides.end()) {
            if (raw.at("origin").at("type") == "relative_deadline") throw std::runtime_error("A configured deadline is not a replaceable operation cost");
            duration = replacement->second;
        }
        if (kind->first == graph::NodeKind::Boundary && duration != 0) throw std::runtime_error("A boundary cannot contain replacement service time");
        const auto name = work_name(raw);
        if (!name.empty()) duration = scaling.apply(name, duration);
        graph.append({ duration, static_cast<std::size_t>(nonnegative(raw.at("resource"))), kind->first });
        if (input.inspect_name && input.inspect_name->empty() && !name.empty()) ++names[name];
        else if (input.inspect_name && !input.inspect_name->empty() && name.contains(*input.inspect_name))
            inspected.push_back({
                {                 "node",                    id },
                {                 "name",                  name },
                { "original_duration_ns", raw.at("duration_ns") },
                {    "bound_duration_ns",              duration }
            });
    }
    for (const auto & raw : serialized.at("edges")) {
        const auto name_of_kind = raw.at(2).get<std::string>();
        const auto kind = std::ranges::find_if(edge_kinds, [&](const auto & item) { return item.second == name_of_kind; });
        if (kind == edge_kinds.end()) throw std::runtime_error("Unsupported recorded dependency kind");
        graph.connect(nonnegative(raw.at(0)), nonnegative(raw.at(1)), kind->first);
    }
    graph.finalize();
    const std::vector<graph::Time> roots(graph.nodes().size(), 0);
    simulation::Execution execution(graph, roots);
    execution.run();
    const auto & result = execution.result();
    std::size_t changed = 0;
    for (std::size_t id = 0; id < nodes.size(); ++id) {
        if (nodes[id].at("origin").at("type") == "relative_deadline") continue;

        changed += result.starts_ns[id] != nodes[id].at("start_ns").get<graph::Time>()
                   || result.completions_ns[id] != nodes[id].at("completion_ns").get<graph::Time>();
    }
    auto requests = Json::array();
    std::optional<graph::Time> formal_begin, formal_end;
    for (const auto & request : selected.cell.at("requests")) {
        const auto start = static_cast<std::size_t>(nonnegative(request.at("start_node")));
        const auto end = static_cast<std::size_t>(nonnegative(request.at("response_node")));
        if (start >= roots.size() || end >= roots.size()) throw std::runtime_error("A recorded HTTP endpoint is outside the graph");
        const auto begin_ns = result.starts_ns[start], end_ns = result.completions_ns[end];
        if (request.at("formal").get<bool>()) {
            if (!formal_begin) formal_begin = begin_ns;
            formal_end = end_ns;
        }
        requests.push_back({
            {     "request",                                                                    request.at("request") },
            {      "formal",                                                                     request.at("formal") },
            {    "start_ns",                                                                                 begin_ns },
            { "response_ns",                                                                                   end_ns },
            {   "replay_ns",                                                                        end_ns - begin_ns },
            { "original_ns", request.at("response_ns").get<graph::Time>() - request.at("start_ns").get<graph::Time>() }
        });
    }
    auto rules = Json::array();
    for (std::size_t id = 0; id < input.node_scale.size(); ++id)
        rules.push_back({
            {          "name",   input.node_scale[id].name },
            {        "factor", input.node_scale[id].factor },
            { "matched_nodes",       scaling.matched()[id] }
        });
    return {
        {                       "status",                                                                                                                "completed" },
        {                   "prediction",                                                                                               selected.prediction.string() },
        {                       "target",                                                                                                   selected.cell.at("name") },
        {                        "nodes",                                                                                                       graph.nodes().size() },
        {                        "edges",                                                                                                       graph.edges().size() },
        {               "executed_nodes",                                                                                                            result.executed },
        {             "overridden_nodes",                                                                                                     input.overrides.size() },
        { "operation_timing_differences",                                                                                                                    changed },
        {                   "node_scale",                                                                                                           std::move(rules) },
        {              "inspected_nodes",                                                                                                       std::move(inspected) },
        {                "service_names",                                                                                                           std::move(names) },
        {                     "requests",                                                                                                        std::move(requests) },
        {            "formal_replay_sec",                                     formal_begin && formal_end ? Json((*formal_end - *formal_begin) / 1e9) : Json(nullptr) },
        {                "qualification", "Recorded target path with explicit costs; no target-trace matching, branch regeneration or normal accuracy qualification" }
    };
}

} // namespace markov::commands
