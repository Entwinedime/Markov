#include "markov/trace_graph/source/regions.hpp"
#include "markov/trace_graph/source/context.hpp"

#include <algorithm>
#include <map>
#include <tuple>

namespace markov::trace_graph::source {

Regions::Regions(const SourceContext & source) : frames_(source.observations().compute_frames.size()), requests_(source.observations().requests.size()) {
    struct Segment {
        Index frame, step;
        ObservedRegion interval;
        Time received_ns;
    };
    const auto & observed = source.observations();
    for (const auto & execution : observed.request_executions) {
        const auto request = std::ranges::find(observed.requests, observed.names[execution.request], &RequestWindow::identity);
        if (request == observed.requests.end() || !request->received || !request->sent) continue;

        auto & ports = requests_[request - observed.requests.begin()];
        const auto & region = execution.region;
        const auto received_ns = observed.points[*request->received].time_ns;
        const auto sent_ns = observed.points[*request->sent].time_ns;
        if (received_ns <= region.begin_ns && region.end_ns <= sent_ns) {
            ports.before_execution = residuals_.size();
            residuals_.push_back({
                { region.resource, received_ns, region.begin_ns, region.record },
                {}
            });
            ports.after_execution = residuals_.size();
            residuals_.push_back({
                { region.resource, region.end_ns, sent_ns, region.record },
                {}
            });
        }
    }
    std::map<Index, std::vector<Segment>> lanes;
    std::map<Index, std::vector<ObservedRegion>> exclusions;
    for (const auto & span : observed.spans) {
        // Unknown waits remain source residuals. Only observed instrumentation
        // and controls with explicit target replacements can leave this cost.
        if (span.role == SpanRole::ProfilerOverhead) exclusions[span.resource].push_back({ span.resource, span.begin_ns, span.end_ns, span.record });
    }
    for (const auto & control : observed.regenerated_controls) exclusions[control.resource].push_back(control);
    for (auto & [resource, intervals] : exclusions) std::ranges::sort(intervals, {}, &ObservedRegion::begin_ns);

    const auto add_residual = [&](ObservedRegion region) {
        ResidualRegion gap{ region, {} };
        auto cursor = region.begin_ns;
        for (const auto & excluded : exclusions[region.resource]) {
            if (excluded.end_ns <= cursor) continue;
            if (excluded.begin_ns >= region.end_ns) break;
            const auto first = std::max(cursor, excluded.begin_ns);
            const auto last = std::min(region.end_ns, excluded.end_ns);
            if (first < last) gap.excluded.push_back({ region.resource, first, last, excluded.record });
            cursor = std::max(cursor, last);
        }
        const auto id = residuals_.size();
        residuals_.push_back(std::move(gap));
        return id;
    };

    for (Index frame = 0; frame < frames_.size(); ++frame) {
        const auto & facts = observed.compute_frames[frame];
        const auto * result = source.frame_result(frame);
        if (!result || !facts.preparation_region || facts.requests.size() != 1) continue;
        const auto request = std::ranges::find(observed.requests, observed.names[facts.requests.front()], &RequestWindow::identity);
        if (request == observed.requests.end()) continue;
        const auto received = request->received ? observed.points[*request->received].time_ns : request->begin_ns;
        lanes[result->run.resource].push_back({ frame, 0, *facts.preparation_region, received });
        lanes[result->run.resource].push_back({ frame, 1, result->run, received });
        if (result->processed) lanes[result->run.resource].push_back({ frame, 2, result->processed->region, received });
    }
    for (auto & [resource, segments] : lanes) {
        std::ranges::sort(segments, {}, [](const Segment & value) { return std::pair(value.interval.begin_ns, value.interval.end_ns); });
        auto previous = segments.front().received_ns;
        for (const auto & segment : segments) {
            const auto begin = std::max(previous, segment.received_ns);
            if (begin <= segment.interval.begin_ns) {
                auto & ports = frames_[segment.frame];
                auto & reference = segment.step == 0 ? ports.before_preparation : segment.step == 1 ? ports.before_run : ports.before_result;
                reference = add_residual({ resource, begin, segment.interval.begin_ns, segment.interval.record });
                const auto & control = observed.compute_frames[segment.frame].preparation_rendezvous;
                if (segment.step == 0 && control && begin <= control->call.begin_ns && control->call.end_ns <= segment.interval.begin_ns) {
                    const auto & call = control->call;
                    ports.control = ControlRegions{ add_residual({ resource, begin, call.begin_ns, call.record }),
                                                    add_residual({ resource, control->all_entered_ns, call.end_ns, call.record }),
                                                    add_residual({ resource, call.end_ns, segment.interval.begin_ns, call.record }) };
                }
            }
            previous = std::max(previous, segment.interval.end_ns);
        }
    }
}

} // namespace markov::trace_graph::source
