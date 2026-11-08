#include "markov/production/projection.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov {
namespace {

struct Boundary {
    std::vector<std::size_t> entering;
    std::vector<std::size_t> touching;
};

class ResourceProjection {
public:
    ResourceProjection(const NativeObservation & observation, CoarseGraph & graph, ProjectionEvidence * evidence)
        : observation_(observation),
          graph_(graph),
          evidence_(evidence) {}

    void project() {
        std::vector<std::vector<std::size_t>> members(observation_.resources.size());
        cuts_.resize(observation_.spans.size());
        for (std::size_t span = 0; span < observation_.spans.size(); ++span) {
            const auto & region = observation_.spans[span];
            members[region.resource].push_back(span);
            cuts_[span] = { region.begin, region.end };
        }
        for (const auto & scope : observation_.scopes) {
            for (const auto & access : scope.kv_accesses) {
                add(access.start);
                add(access.finish);
            }
            for (const auto & access : scope.slot_accesses) {
                add(access.start);
                add(access.finish);
            }
            for (const auto & port : scope.ports) {
                add(port.at);
                if (port.wait) add(port.wait->begin);
                if (port.rendezvous) add(*port.rendezvous);
            }
        }
        for (const auto & edge : observation_.dependencies) {
            add(edge.source);
            add(edge.target);
        }

        for (std::size_t resource = 0; resource < members.size(); ++resource) {
            const auto & native = observation_.resources[resource];
            if (!members[resource].empty() && !native.observed) throw std::invalid_argument("required native resource was not observed");

            if (native.descriptor.serial) serial(resource, members[resource]);
            else {
                for (const auto span : members[resource]) service(span);
            }
        }
    }

    std::size_t event(NativeAnchor at) const {
        const auto resource = observation_.spans[at.span].resource;
        if (observation_.resources[resource].descriptor.serial) return serial_events_.at({ resource, at.order });
        return service_events_.at(at);
    }

private:
    void add(NativeAnchor at) { cuts_[at.span].insert(at.order); }

    void interval(std::size_t resource, std::optional<std::size_t> span, NativeOrder begin, NativeOrder end, std::size_t start, std::size_t finish) {
        if (span) {
            const auto & region = observation_.spans[*span];
            const bool timed = region.kind != WorkKind::Wait && region.kind != WorkKind::Boundary;
            const bool payload = region.kind == WorkKind::Host || observation_.resources[resource].has_payload(begin, end);
            if (timed && payload) {
                graph_.work.push_back({ region.scope, resource, region.kind, start, finish });
                if (evidence_) evidence_->work_spans.push_back(*span);
                return;
            }
        }
        graph_.edges.push_back({ start, finish, Relation::Order });
    }

    void serial(std::size_t resource, const std::vector<std::size_t> & spans) {
        std::map<NativeOrder, Boundary> boundaries;
        for (const auto span : spans) {
            const auto & region = observation_.spans[span];
            for (const auto position : cuts_[span]) boundaries[position];
            if (region.begin != region.end) boundaries[region.begin].entering.push_back(span);
            boundaries[region.begin].touching.push_back(span);
            boundaries[region.end].touching.push_back(span);
        }

        std::vector<std::size_t> active;
        std::optional<std::size_t> previous;
        NativeOrder previous_order = 0;
        for (auto & [position, boundary] : boundaries) {

            // A touching boundary belongs to both sides; moving it into only the entering child would lose an import.
            const auto preceding = active.empty() ? std::nullopt : std::optional(active.back());
            std::optional<std::size_t> owner;
            if (preceding) owner = observation_.spans[*preceding].scope;

            while (!active.empty() && observation_.spans[active.back()].end == position) active.pop_back();
            std::ranges::sort(boundary.entering, [this](auto left, auto right) {
                const auto & first = observation_.spans[left];
                const auto & second = observation_.spans[right];
                if (first.end != second.end) return first.end > second.end;
                if (first.scope != second.scope) return first.scope < second.scope;
                return first.kind != WorkKind::Wait && second.kind == WorkKind::Wait;
            });
            for (const auto span : boundary.entering) {
                const auto & entering = observation_.spans[span];
                if (!active.empty()) {
                    const auto & parent = observation_.spans[active.back()];
                    if (entering.end > parent.end || !graph_.contains(parent.scope, entering.scope))
                        throw std::invalid_argument("serial native spans overlap without a nested semantic owner");
                }
                active.push_back(span);
            }
            if (!active.empty()) {
                const auto scope = observation_.spans[active.back()].scope;
                owner = owner ? graph_.common_scope(*owner, scope) : scope;
            }
            for (const auto span : boundary.touching) {
                const auto scope = observation_.spans[span].scope;
                owner = owner ? graph_.common_scope(*owner, scope) : scope;
            }

            const auto current = graph_.events.size();
            graph_.events.push_back({ *owner, resource });
            if (evidence_) evidence_->events.push_back({ boundary.touching.empty() ? active.back() : boundary.touching.front(), position });
            serial_events_.emplace(std::pair{ resource, position }, current);
            if (previous) interval(resource, preceding, previous_order, position, *previous, current);
            previous = current;
            previous_order = position;
        }
        if (!active.empty()) throw std::invalid_argument("native resource span has no closing boundary");
    }

    void service(std::size_t span) {
        const auto & region = observation_.spans[span];
        std::optional<std::size_t> previous;
        NativeOrder previous_order = 0;
        for (const auto position : cuts_[span]) {
            const auto current = graph_.events.size();
            graph_.events.push_back({ region.scope, region.resource });
            if (evidence_) evidence_->events.push_back({ span, position });
            service_events_.emplace(NativeAnchor{ span, position }, current);
            if (previous) interval(region.resource, span, previous_order, position, *previous, current);
            previous = current;
            previous_order = position;
        }
    }

    const NativeObservation & observation_;
    CoarseGraph & graph_;
    ProjectionEvidence * evidence_;
    std::vector<std::set<NativeOrder>> cuts_;
    std::map<std::pair<std::size_t, NativeOrder>, std::size_t> serial_events_;
    std::map<NativeAnchor, std::size_t> service_events_;
};

} // namespace

nlohmann::json ProjectionEvidence::document(const NativeObservation & observation) const {
    if (!observation.measurements) throw std::invalid_argument("projection evidence requires interpreted boundary measurements");
    const auto & measurements = *observation.measurements;
    nlohmann::json result{
        {      "clock",                                           measurements.clock },
        {  "window_ns", { measurements.window_begin_ns, measurements.window_end_ns } },
        {     "events",                                      nlohmann::json::array() },
        { "work_spans",                                                   work_spans }
    };
    for (const auto & event : events) {
        const auto resource = observation.spans[event.span].resource;
        const auto time = measurements.coordinates.at({ resource, event.order });
        result["events"].push_back({
            {    "span",                                             event.span },
            {   "order",                                            event.order },
            { "time_ns", time ? nlohmann::json(*time) : nlohmann::json(nullptr) }
        });
    }
    return result;
}

CoarseGraph CoarseProjection::project(const NativeObservation & observation, ProjectionEvidence * evidence) {
    if (evidence) *evidence = {};
    CoarseGraph graph;
    graph.environment = observation.environment;
    for (const auto & resource : observation.resources) graph.resources.push_back(resource.descriptor);
    for (const auto & scope : observation.scopes) graph.scopes.push_back({ scope.info, {}, {}, scope.communication });

    ResourceProjection projection(observation, graph, evidence);
    projection.project();
    for (std::size_t scope = 0; scope < observation.scopes.size(); ++scope) {
        for (const auto & access : observation.scopes[scope].kv_accesses)
            graph.scopes[scope].kv_accesses.push_back({ access.value, projection.event(access.start), projection.event(access.finish) });
        for (const auto & access : observation.scopes[scope].slot_accesses)
            graph.scopes[scope].slot_accesses.push_back({ access.value, projection.event(access.start), projection.event(access.finish) });
        for (const auto & port : observation.scopes[scope].ports) {
            GraphPort boundary{ port.name, port.direction, port.use, port.relation, projection.event(port.at), port.tensor };
            if (port.wait) boundary.wait = StreamWait<std::size_t>{ port.wait->resource, projection.event(port.wait->begin) };
            if (port.rendezvous) boundary.rendezvous = projection.event(*port.rendezvous);
            boundary.signal = port.signal;
            graph.scopes[scope].ports.push_back(std::move(boundary));
        }
    }
    for (const auto & dependency : observation.dependencies) {
        const auto source = projection.event(dependency.source);
        const auto target = projection.event(dependency.target);
        if (source != target) graph.edges.push_back({ source, target, dependency.kind });
    }

    std::ranges::sort(graph.edges);
    graph.edges.erase(std::unique(graph.edges.begin(), graph.edges.end()), graph.edges.end());
    graph.validate();
    return graph;
}

} // namespace markov
