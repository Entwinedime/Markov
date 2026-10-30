#include "markov/commands/replay.hpp"
#include "markov/commands/trajectory.hpp"

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace markov::commands {

nlohmann::json replay_report(const adapters::PreparedReplay & input) {
    using namespace trace_graph;
    using Json = nlohmann::json;
    if (std::holds_alternative<adapters::PredictedTrajectory>(input.input)) return trajectory_replay_report(input);
    const auto & prepared = std::get<adapters::PreparedSource>(input.input);
    const auto & source = prepared.context;
    const auto & observed = source.observations();
    const auto & original = source.graph();
    if (!input.overrides.empty() && input.overrides.rbegin()->first >= original.graph.nodes().size())
        throw std::runtime_error("Replay override node is outside the source graph");
    cost::NodeScale scaling(input.node_scale);
    graph::Graph transformed;
    auto inspected = Json::array();
    std::map<std::string, std::size_t> service_names;
    std::vector<graph::Time> available(original.graph.nodes().size(), 0);
    for (std::size_t id = 0; id < original.graph.nodes().size(); ++id) {
        auto node = original.graph.nodes()[id];
        if (const auto replacement = input.overrides.find(id); replacement != input.overrides.end()) node.duration_ns = replacement->second;
        if (node.kind == graph::NodeKind::Boundary && node.duration_ns != 0) throw std::runtime_error("A boundary cannot contain replacement service time");
        const auto span = original.node_origins[id].span;
        if (node.kind == graph::NodeKind::Service && span < observed.spans.size()) {
            const auto & name = observed.names[observed.spans[span].name];
            node.duration_ns = scaling.apply(name, node.duration_ns);
            if (input.inspect_name && input.inspect_name->empty()) ++service_names[name];
            else if (input.inspect_name && name.contains(*input.inspect_name))
                inspected.push_back({
                    {                 "node",                                     id },
                    {                 "name",                                   name },
                    {             "resource",                          node.resource },
                    { "observed_duration_ns", original.graph.nodes()[id].duration_ns },
                    {    "bound_duration_ns",                       node.duration_ns }
                });
        }
        transformed.append(node);

        // Only roots retain the source's time origin. Later operations move
        // with their dependencies when a service cost changes.
        if (original.graph.incoming_counts()[id] == 0) available[id] = original.node_origins[id].begin_ns - source.origin_ns();
    }
    for (const auto & edge : original.graph.edges()) transformed.connect(edge.from, edge.to, edge.kind);
    transformed.finalize();
    simulation::Execution execution(transformed, available);
    execution.run();
    const auto & result = execution.result();

    Json requests = Json::array();
    std::optional<graph::Time> formal_begin, formal_end;
    for (const auto & endpoint : original.requests) {
        const auto begin = result.starts_ns[endpoint.start];
        const auto end = result.completions_ns[endpoint.completion];
        if (endpoint.request.formal) {
            if (!formal_begin) formal_begin = begin;
            formal_end = end;
        }
        requests.push_back({
            {     "request",                           endpoint.request.identity },
            {      "formal",                             endpoint.request.formal },
            {   "replay_ns",                                         end - begin },
            { "observed_ns", endpoint.request.end_ns - endpoint.request.begin_ns }
        });
    }
    Json rules = Json::array();
    for (std::size_t id = 0; id < input.node_scale.size(); ++id)
        rules.push_back({
            {          "name",   input.node_scale[id].name },
            {        "factor", input.node_scale[id].factor },
            { "matched_nodes",       scaling.matched()[id] }
        });
    return {
        {            "status",                                                                                                         "completed" },
        {          "manifest",                                                                                     prepared.manifest.path.string() },
        {             "nodes",                                                                                          transformed.nodes().size() },
        {             "edges",                                                                                          transformed.edges().size() },
        {    "executed_nodes",                                                                                                     result.executed },
        {        "node_scale",                                                                                                    std::move(rules) },
        {  "overridden_nodes",                                                                                              input.overrides.size() },
        {          "requests",                                                                                                 std::move(requests) },
        {   "inspected_nodes",                                                                                                std::move(inspected) },
        {     "service_names",                                                                                            std::move(service_names) },
        { "formal_replay_sec",                              formal_begin && formal_end ? Json((*formal_end - *formal_begin) / 1e9) : Json(nullptr) },
        {     "qualification", "Static source-DAG cost sensitivity; no target operation mapping, branch regeneration or normal E2E accuracy claim" }
    };
}

} // namespace markov::commands
