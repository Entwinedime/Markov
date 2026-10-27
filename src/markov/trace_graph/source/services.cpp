#include "markov/trace_graph/source/services.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace markov::trace_graph::source {
namespace {

struct Lane {
    std::vector<Index> work;
    std::vector<Index> device_spans;
    std::vector<Index> submissions;
};

} // namespace

std::vector<DeviceService> bind_device_services(const Observations & observations, const SourceGraph & graph) {
    std::vector<DeviceService> result(observations.work_observations.size());
    std::vector<Lane> lanes(observations.resources.size());
    for (Index id = 0; id < observations.work_observations.size(); ++id) lanes[observations.work_observations[id].resource].work.push_back(id);
    for (Index id = 0; id < observations.spans.size(); ++id) {
        const auto & span = observations.spans[id];
        if (observations.resources[span.resource].kind == ResourceKind::DeviceStream) lanes[span.resource].device_spans.push_back(id);
    }
    for (Index id = 0; id < observations.dependencies.size(); ++id) {
        const auto & link = observations.dependencies[id];
        const auto & before = observations.points[link.before];
        const auto & after = observations.points[link.after];
        if (link.kind != DependencyKind::Synchronization && observations.resources[before.resource].kind == ResourceKind::CpuThread
            && observations.resources[after.resource].kind == ResourceKind::DeviceStream)
            lanes[before.resource].submissions.push_back(id);
    }
    for (auto & lane : lanes) {
        std::ranges::sort(lane.device_spans, {}, [&](Index id) { return observations.spans[id].begin_ns; });
    }

    constexpr auto unclaimed = std::numeric_limits<Index>::max();
    constexpr auto ambiguous = unclaimed - 1;
    std::vector<Index> span_owner(observations.spans.size(), unclaimed);
    for (auto & lane : lanes) {
        if (lane.work.empty()) continue;

        std::ranges::sort(lane.work, {}, [&](Index id) {
            const auto & call = observations.work_observations[id];
            return std::tuple(call.begin_ns, call.end_ns, call.record.file, call.record.position);
        });
        std::ranges::sort(lane.submissions, {}, [&](Index id) { return observations.points[observations.dependencies[id].before].time_ns; });

        // Sweep the CPU envelopes once; nesting and shared primitive probes
        // remain ambiguous instead of charging the same launch twice.
        std::set<std::pair<Time, Index>> active;
        Index next = 0;
        for (const auto submission : lane.submissions) {
            const auto & link = observations.dependencies[submission];
            const auto time = observations.points[link.before].time_ns;
            while (next < lane.work.size() && observations.work_observations[lane.work[next]].begin_ns <= time) {
                const auto id = lane.work[next];
                active.emplace(observations.work_observations[id].end_ns, id);
                ++next;
            }
            while (!active.empty() && active.begin()->first <= time) active.erase(active.begin());
            if (active.empty()) continue;
            const auto work = active.begin()->second;
            if (active.size() != 1 || observations.work_descriptions[observations.work_observations[work].description].observed.size() != 1) {
                for (const auto & [end, id] : active) result[id].issue = ServiceIssue::SharedProbe;
                continue;
            }

            const auto & port = observations.points[link.after];
            const auto & candidates = lanes[port.resource].device_spans;
            const auto at = std::ranges::lower_bound(candidates, port.time_ns, {}, [&](Index id) { return observations.spans[id].begin_ns; });
            if (at == candidates.end() || observations.spans[*at].begin_ns != port.time_ns) {
                result[work].issue = ServiceIssue::UnmatchedDevice;
                continue;
            }
            const auto after = std::next(at);
            if (after != candidates.end() && observations.spans[*after].begin_ns == port.time_ns) {
                result[work].issue = ServiceIssue::AmbiguousOwner;
                continue;
            }

            auto & owner = span_owner[*at];
            if (owner == unclaimed) owner = work;
            else if (owner != work) {
                if (owner != ambiguous) result[owner].issue = ServiceIssue::AmbiguousOwner;
                result[work].issue = ServiceIssue::AmbiguousOwner;
                owner = ambiguous;
            }
        }
    }

    // Read the source's exclusive partition, not parent device wall envelopes.
    for (graph::NodeId id = 0; id < graph.graph.nodes().size(); ++id) {
        const auto span = graph.node_origins[id].span;
        if (span == unclaimed || span_owner[span] >= result.size()) continue;

        const auto & node = graph.graph.nodes()[id];
        if (node.kind == graph::NodeKind::Boundary) continue;
        auto & service = result[span_owner[span]];
        if (node.kind != graph::NodeKind::Service) service.issue = ServiceIssue::NonServiceDevice;
        else service.nodes.push_back(id);
    }
    return result;
}

} // namespace markov::trace_graph::source
