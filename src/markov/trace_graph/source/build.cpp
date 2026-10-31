#include "markov/trace_graph/source/build.hpp"
#include "markov/trace_graph/source/services.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace markov::trace_graph::source {
namespace {

constexpr auto no_span = std::numeric_limits<Index>::max();

struct Boundary {
    Time time;
    Index span;
    bool begin;
};

struct LaneInput {
    std::vector<Boundary> boundaries;
    std::vector<Index> points;
};

class LaneBuilder {
public:
    LaneBuilder(const Observations & observations, Index resource, LaneInput & input, SourceGraph & output)
        : observations_(observations),
          resource_(resource),
          input_(input),
          output_(output) {}

    void build() {
        std::ranges::sort(input_.boundaries, {}, &Boundary::time);
        std::ranges::sort(input_.points, [&](Index a, Index b) { return observations_.points[a].time_ns < observations_.points[b].time_ns; });
        Index boundary = 0, point = 0;
        std::optional<Time> previous;
        while (boundary < input_.boundaries.size() || point < input_.points.size()) {
            const auto boundary_time = boundary < input_.boundaries.size() ? input_.boundaries[boundary].time : std::numeric_limits<Time>::max();
            const auto point_time = point < input_.points.size() ? observations_.points[input_.points[point]].time_ns : std::numeric_limits<Time>::max();
            const auto time = std::min(boundary_time, point_time);
            if (previous && time > *previous) observe_interval(*previous, time);
            else if (!previous) {
                // An incoming flow must not erase this lane's first observed
                // availability. Later nodes still follow costs and dependencies.
                frontier_ = { append({ time, 0, no_span, graph::NodeKind::Boundary }) };
            }

            if (point < input_.points.size() && observations_.points[input_.points[point]].time_ns == time) {
                flush_interval();
                std::vector<graph::NodeId> next_frontier;
                std::map<std::pair<Index, Index>, graph::NodeId> record_ports;
                while (point < input_.points.size() && observations_.points[input_.points[point]].time_ns == time) {
                    const auto id = input_.points[point++];
                    const auto & record = observations_.points[id].record;
                    const auto key = std::pair(record.file, record.position);
                    const auto [port, inserted] = record_ports.try_emplace(key, 0);
                    if (inserted) {
                        port->second = append({ time, 0, no_span, graph::NodeKind::Boundary });
                        next_frontier.push_back(port->second);
                    }
                    else ++output_.audit["reused_record_boundary_ports"];

                    // One record's coincident ports denote the same fact; equal
                    // timestamps on different records do not prove identity.
                    output_.point_nodes[id] = port->second;
                }
                frontier_ = std::move(next_frontier);
            }
            while (boundary < input_.boundaries.size() && input_.boundaries[boundary].time == time) {
                const auto & value = input_.boundaries[boundary++];
                const auto & span = observations_.spans[value.span];
                const auto key = std::tuple(-span.begin_ns, span.end_ns, value.span);
                if (value.begin) active_.insert(key);
                else active_.erase(key);
            }
            previous = time;
        }
        flush_interval();
    }

private:
    struct Interval {
        Time begin_ns;
        Time duration_ns;
        Index source_span;
        graph::NodeKind kind;
    };

    graph::NodeId append(const Interval & node) {
        const auto id = output_.append(node.begin_ns, node.duration_ns, resource_, node.source_span, node.kind);
        for (const auto prior : frontier_) output_.graph.connect(prior, id, graph::EdgeKind::ResourceOrder);
        return id;
    }

    void observe_interval(Time begin, Time end) {
        const auto span = active_.empty() ? no_span : std::get<2>(*active_.begin());
        auto kind = graph::NodeKind::ResidualGap;
        if (span != no_span) {
            switch (observations_.spans[span].role) {
            case SpanRole::Service:
                kind = graph::NodeKind::Service;
                break;
            case SpanRole::Wait:
            case SpanRole::StreamWait:
            case SpanRole::EventWait:
                kind = graph::NodeKind::ObservedWait;
                break;
            case SpanRole::ProfilerOverhead:
                kind = graph::NodeKind::ProfilerOverhead;
                break;
            }
        }
        if (pending_ && pending_->source_span == span && pending_->kind == kind && pending_->begin_ns + pending_->duration_ns == begin) {
            pending_->duration_ns += end - begin;
            return;
        }

        flush_interval();
        pending_ = Interval{ begin, end - begin, span, kind };
    }

    void flush_interval() {
        if (!pending_) return;
        frontier_ = { append(*pending_) };
        pending_.reset();
    }

    const Observations & observations_;
    Index resource_;
    LaneInput & input_;
    SourceGraph & output_;
    std::set<std::tuple<Time, Time, Index>> active_;
    std::vector<graph::NodeId> frontier_;
    std::optional<Interval> pending_;
};

} // namespace

graph::NodeId SourceGraph::append(Time begin_ns, Time duration_ns, Index resource, Index span, graph::NodeKind kind) {
    const auto id = graph.append({ duration_ns, resource, kind });
    node_origins.push_back({ begin_ns, span });
    return id;
}

SourceGraph build_source_graph(const Observations & observations) {
    SourceGraph result;
    const auto client = std::ranges::find_if(observations.resources, [](const Resource & resource) {
        return resource.kind == ResourceKind::Client && resource.identity == "http_client";
    });
    const auto client_resource = static_cast<Index>(client - observations.resources.begin());
    result.point_nodes.resize(observations.points.size());
    std::map<Index, LaneInput> lanes;
    for (Index i = 0; i < observations.spans.size(); ++i) {
        const auto & span = observations.spans[i];
        auto & lane = lanes[span.resource];
        lane.boundaries.push_back({ span.begin_ns, i, true });
        lane.boundaries.push_back({ span.end_ns, i, false });
    }
    for (Index i = 0; i < observations.points.size(); ++i) lanes[observations.points[i].resource].points.push_back(i);
    for (auto & [resource, lane] : lanes) LaneBuilder(observations, resource, lane, result).build();

    for (const auto & dependency : observations.dependencies) {
        auto kind = graph::EdgeKind::ObservedFlow;
        if (dependency.kind == DependencyKind::Submission) kind = graph::EdgeKind::Submission;
        if (dependency.kind == DependencyKind::Synchronization) kind = graph::EdgeKind::Synchronization;
        result.graph.connect(result.point_nodes[dependency.before], result.point_nodes[dependency.after], kind);
    }
    if (!observations.device_completion_fences.empty()) {
        std::vector<ObservedRegion> regions;
        for (const auto & fence : observations.device_completion_fences) regions.push_back(fence.submissions);
        const auto bindings = bind_device_services(observations, result, regions);
        result.audit["observed_device_completion_fences"] = regions.size();
        for (Index id = 0; id < bindings.size(); ++id) {
            if (!bindings[id].available()) {
                ++result.audit["device_completion_fences_without_unique_service"];
                continue;
            }
            std::map<Index, graph::NodeId> frontiers;
            for (const auto node : bindings[id].nodes) frontiers[result.graph.nodes()[node].resource] = node;
            const auto completed = result.point_nodes[observations.device_completion_fences[id].completion];
            for (const auto & [resource, producer] : frontiers) {
                // Lane order already contains earlier services. Gate the
                // region's return, never its start or its observed wait cost.
                result.graph.connect(producer, completed, graph::EdgeKind::Synchronization);
                ++result.audit["device_completion_fence_frontiers"];
                if (result.node_origins[producer].begin_ns + result.graph.nodes()[producer].duration_ns > result.node_origins[completed].begin_ns)
                    ++result.audit["device_completion_fence_clock_order_violations"];
            }
        }
    }
    std::optional<graph::NodeId> previous_http;
    Time previous_end = 0;
    for (const auto & request : observations.requests) {
        if (!request.received || !request.sent) {
            ++result.audit["unbound_http_requests"];
            continue;
        }
        const auto received = observations.points[*request.received].time_ns;
        const auto sent = observations.points[*request.sent].time_ns;
        if (received < request.begin_ns || sent < received || sent > request.end_ns) {
            ++result.audit["http_boundary_order_violations"];
            continue;
        }

        const auto start = result.append(request.begin_ns, 0, client_resource, no_span, graph::NodeKind::Boundary);
        if (previous_http) {
            const auto pause = result.append(previous_end, request.begin_ns - previous_end, client_resource, no_span, graph::NodeKind::ResidualGap);
            result.graph.connect(*previous_http, pause, graph::EdgeKind::ClientOrder);
            result.graph.connect(pause, start, graph::EdgeKind::ClientOrder);
        }
        const auto ingress = result.append(request.begin_ns, received - request.begin_ns, client_resource, no_span, graph::NodeKind::ResidualGap);
        const auto response = result.append(sent, request.end_ns - sent, client_resource, no_span, graph::NodeKind::ResidualGap);
        result.graph.connect(start, ingress, graph::EdgeKind::ClientOrder);
        result.graph.connect(ingress, result.point_nodes[*request.received], graph::EdgeKind::ClientOrder);
        result.graph.connect(result.point_nodes[*request.sent], response, graph::EdgeKind::ClientOrder);
        result.requests.push_back({ request, start, ingress, response });
        previous_http = response;
        previous_end = request.end_ns;
    }
    result.graph.finalize();
    return result;
}

} // namespace markov::trace_graph::source
