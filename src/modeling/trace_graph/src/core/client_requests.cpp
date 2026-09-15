/** @file Connect client completion to subsequent request availability. */
#include "markov/trace_graph/core/client_requests.hpp"
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

namespace markov::trace_graph::core {

ClientRequestChain connect_client_requests(DagGraph & graph, std::span<const ClientRequestTiming> requests) {
    if (requests.empty()) return {};
    struct Boundaries { std::vector<size_t> received, sent; };
    std::unordered_map<std::string, Boundaries> boundaries;
    for (const auto & node : graph.nodes()) {
        if (!node.active) continue;
        const auto & event = graph.event_for_node(node.id);
        const bool received = event.arg("observed_interval") == "runtime.request.socket_received";
        const bool sent = event.arg("observed_interval") == "runtime.response.scheduler_send" && event.arg("interval_boundary") == "end";
        if (!received && !sent) continue;
        for (const auto & id : nlohmann::json::parse(event.arg("request_ids"))) {
            auto & found = boundaries[id.get<std::string>()];
            (received ? found.received : found.sent).push_back(node.id);
        }
    }
    // Preflight before mutating: unavailable observations cannot leave half a chain.
    uint64_t previous_end = 0;
    std::unordered_set<std::string> ids;
    for (const auto & request : requests) {
        if (request.request_id.empty() || !ids.insert(request.request_id).second) return {"duplicate_or_missing_request_id", {}};
        if (request.start_us < previous_end || request.end_us <= request.start_us) return {"requests_not_serial", {}};
        const auto & boundary = boundaries[request.request_id];
        if (boundary.received.size() != 1 || boundary.sent.size() != 1) return {"missing_or_ambiguous_request_boundary", {}};
        const auto received = graph.event_for_node(boundary.received.front()).ts;
        const auto sent = graph.event_for_node(boundary.sent.front()).ts;
        if (request.frontend_end_us < request.start_us || request.frontend_end_us > received || received > sent || sent > request.end_us)
            return {"request_boundary_order_not_proven", {}};
        previous_end = request.end_us;
    }

    ClientRequestChain result{"connected", {}};
    size_t previous_completion = DagNode::kNoNode;
    previous_end = requests.front().start_us;
    for (const auto & request : requests) {
        const auto & boundary = boundaries.at(request.request_id);
        const auto add = [&](std::string_view stage, uint64_t duration, uint64_t ts) {
            const auto id = graph.add_synthetic_node({.name = "client." + std::string(stage), .category = "http_client",
                .lane_key = "HTTP_CLIENT", .duration = duration, .attrs = {{"request_id", request.request_id}, {"cost_source", "base_observation"}}});
            graph.mutable_event_for_node(id).ts = ts;
            return id;
        };
        const auto start = add("before_request", request.start_us - previous_end, previous_end);
        const auto frontend = add("frontend", request.frontend_end_us - request.start_us, request.start_us);
        const auto sent = boundary.sent.front();
        const auto completion = add("response", request.end_us - graph.event_for_node(sent).ts, graph.event_for_node(sent).ts);
        if (previous_completion != DagNode::kNoNode) graph.add_edge(previous_completion, start, DagEdgeKind::Sync);
        graph.add_edge(start, frontend, DagEdgeKind::Sync);
        graph.add_edge(frontend, boundary.received.front(), DagEdgeKind::Sync);
        graph.add_edge(sent, completion, DagEdgeKind::Sync);
        result.requests.push_back({request.request_id, start, completion});
        previous_completion = completion;
        previous_end = request.end_us;
    }
    return result;
}

} // namespace markov::trace_graph::core
