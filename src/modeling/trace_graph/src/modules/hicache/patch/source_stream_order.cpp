#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::patch {
namespace {

std::pair<uint64_t, uint64_t> submission_interval(const core::TraceEvent & event) {
    if (!event.arg("pre_window_enqueue_tid").empty())
        return { core::parse_u64(event.arg("pre_window_enqueue_start_us")).value(), core::parse_u64(event.arg("pre_window_enqueue_end_us")).value() };

    return { event.ts, core::checked_add_u64(event.ts, event.dur, "Caller submission end exceeds uint64 range") };
}

} // namespace

std::optional<size_t> HiCacheDeviceStreamOrder::previous(size_t node) const {
    const auto position = positions.at(node);
    return position ? std::optional<size_t>{ nodes[position - 1] } : std::nullopt;
}

std::optional<size_t> HiCacheDeviceStreamOrder::next(size_t node) const {
    const auto position = positions.at(node) + 1;
    return position < nodes.size() ? std::optional<size_t>{ nodes[position] } : std::nullopt;
}

const HiCacheDeviceStreamOrder & HiCacheSourceDagIndex::device_stream_order(size_t sample) const {
    const auto & selected = graph_.node(sample);
    // The sample identifies a physical resource, not a dependency endpoint.
    // A replaced call may remove it while its rank/lane remains usable.
    if (selected.is_cpu) throw std::invalid_argument("Device stream order requires a device resource sample");
    const auto key = std::pair{ selected.gpu_id, selected.lane_id };
    if (const auto found = device_stream_orders_.find(key); found != device_stream_orders_.end()) return found->second;
    std::unordered_map<size_t, size_t> indegrees;
    for (const auto & node : graph_.nodes())
        if (node.active && !node.is_cpu && node.gpu_id == key.first && node.lane_id == key.second) indegrees.emplace(node.id, 0);
    for (auto & [node, count] : indegrees)
        for (const auto id : incoming_edge_ids(node)) {
            const auto & edge = graph_.edge(id);
            if (!edge.active || edge.kind != core::DagEdgeKind::Stream || !graph_.node(edge.src).active) continue;
            if (!indegrees.contains(edge.src)) throw std::runtime_error("Stream dependency crosses its physical rank/lane");
            ++count;
        }
    std::vector<size_t> ready;
    for (const auto & [node, count] : indegrees)
        if (!count) ready.push_back(node);
    HiCacheDeviceStreamOrder order;
    order.nodes.reserve(indegrees.size());
    while (!ready.empty()) {
        if (ready.size() != 1) throw std::runtime_error("Device stream has no unique dependency order");
        const auto node = ready.back();
        ready.pop_back();
        order.positions.emplace(node, order.nodes.size());
        order.nodes.push_back(node);
        for (const auto id : outgoing_edge_ids(node)) {
            const auto & edge = graph_.edge(id);
            if (!edge.active || edge.kind != core::DagEdgeKind::Stream || !graph_.node(edge.dst).active) continue;
            const auto next = indegrees.find(edge.dst);
            if (next == indegrees.end()) throw std::runtime_error("Stream dependency crosses its physical rank/lane");
            if (--next->second == 0) ready.push_back(edge.dst);
        }
    }
    if (order.nodes.size() != indegrees.size()) throw std::runtime_error("Device stream contains a dependency cycle");
    return device_stream_orders_.emplace(key, std::move(order)).first->second;
}

const std::vector<std::vector<size_t>> & HiCacheSourceDagIndex::device_submission_anchors(size_t sample, const std::string & pid,
                                                                                          const std::string & tid) const {
    const auto & resource = graph_.node(sample);
    const auto key = std::tuple{ resource.gpu_id, resource.lane_id, pid, tid };
    if (const auto found = device_submission_anchors_.find(key); found != device_submission_anchors_.end()) return found->second;

    std::vector<std::vector<size_t>> result;
    for (const auto current : device_stream_order(sample).nodes) {
        std::set<size_t> anchors, searched;
        std::vector<size_t> pending{ current };
        while (!pending.empty()) {
            const auto node = pending.back();
            pending.pop_back();
            if (!searched.insert(node).second) continue;
            const auto & event = graph_.event_for_node(node);
            if (graph_.node(node).is_cpu && event.pid == pid && (event.tid == tid || event.arg("pre_window_enqueue_tid") == tid)) {
                anchors.insert(node);
                continue;
            }
            for (const auto id : incoming_edge_ids(node)) {
                const auto & edge = graph_.edge(id);
                if (edge.active && edge.kind == core::DagEdgeKind::Correlation && graph_.node(edge.src).active) pending.push_back(edge.src);
            }
        }
        result.emplace_back(anchors.begin(), anchors.end());
    }
    return device_submission_anchors_.emplace(key, std::move(result)).first->second;
}

bool HiCacheSourceDagIndex::device_submitted_in_call(size_t node, const std::string & pid, const std::string & tid, uint64_t begin_us, uint64_t end_us) const {
    const auto & order = device_stream_order(node);
    const auto & anchors = device_submission_anchors(node, pid, tid).at(order.positions.at(node));
    if (anchors.empty()) {
        const auto & event = graph_.event_for_node(node);
        if (event.arg("pre_window_submit_pid") != pid || event.arg("pre_window_submit_tid") != tid) return false;

        const auto first = core::parse_u64(event.arg("pre_window_submit_start_us")).value();
        const auto last = core::parse_u64(event.arg("pre_window_submit_end_us")).value();
        return first >= begin_us && first < end_us && last <= end_us;
    }

    for (const auto anchor : anchors) {
        const auto [first, last] = submission_interval(graph_.event_for_node(anchor));
        if (first < begin_us || first >= end_us || last > end_us) return false;
    }
    return true;
}

HiCacheDeviceSubmissionCut HiCacheSourceDagIndex::device_submission_cut(size_t sample, const std::string & pid, const std::string & tid, uint64_t at_us) const {
    const auto & resource = graph_.node(sample);
    const auto key = std::tuple{ resource.gpu_id, resource.lane_id, pid, tid };
    auto found = device_submission_timelines_.find(key);
    if (found == device_submission_timelines_.end()) {
        DeviceSubmissionTimeline timeline;
        const auto & submissions = device_submission_anchors(sample, pid, tid);
        const auto & order = device_stream_order(sample);
        for (size_t ordinal = 0; ordinal < submissions.size(); ++ordinal) {
            const auto & anchors = submissions[ordinal];
            if (anchors.empty()) {
                const auto & event = graph_.event_for_node(order.nodes[ordinal]);
                if (event.arg("pre_window_submit_pid") == pid) {
                    // This native submission completed before the formal window,
                    // hence before every retained caller's insertion point.
                    // Its CPU service stays outside the executable graph.
                    const auto first = core::parse_u64(event.arg("pre_window_submit_start_us")).value();
                    const auto last = core::parse_u64(event.arg("pre_window_submit_end_us")).value();
                    timeline.starts.emplace_back(first, ordinal);
                    timeline.finishes.push_back({
                        { last, last == first },
                        ordinal
                    });
                }
                else timeline.unanchored.push_back(ordinal);
                continue;
            }

            uint64_t first_start = std::numeric_limits<uint64_t>::max(), last_start = 0, last_end = 0;
            for (const auto node : anchors) {
                const auto [first, last] = submission_interval(graph_.event_for_node(node));
                first_start = std::min(first_start, first);
                last_start = std::max(last_start, first);
                last_end = std::max(last_end, last);
            }
            timeline.starts.emplace_back(first_start, ordinal);
            timeline.finishes.push_back({
                { last_end, last_end == last_start },
                ordinal
            });
        }

        std::ranges::sort(timeline.starts);
        std::ranges::sort(timeline.finishes);
        size_t last_before = 0;
        for (const auto & [end, ordinal] : timeline.finishes) {
            last_before = std::max(last_before, ordinal);
            timeline.last_before_prefix.push_back(last_before);
        }
        timeline.first_after_suffix.resize(timeline.starts.size());
        size_t first_after = submissions.size();
        for (size_t i = timeline.starts.size(); i > 0; --i) {
            first_after = std::min(first_after, timeline.starts[i - 1].second);
            timeline.first_after_suffix[i - 1] = first_after;
        }
        found = device_submission_timelines_.emplace(key, std::move(timeline)).first;
    }

    const auto & timeline = found->second;
    const auto before_end = std::ranges::upper_bound(timeline.finishes, std::pair{ at_us, false }, {}, [](const auto & entry) { return entry.first; });
    const auto after_begin = std::ranges::lower_bound(timeline.starts, at_us, {}, [](const auto & entry) { return entry.first; });
    const auto before_count = static_cast<size_t>(before_end - timeline.finishes.begin());
    const auto after_index = static_cast<size_t>(after_begin - timeline.starts.begin());
    // Before and after are disjoint. Any unclassified anchored operation
    // crosses the cut; timestamps need not be monotone in Stream order.
    if (before_count != after_index) throw std::runtime_error("Write insertion overlaps a CPU submission");

    HiCacheDeviceSubmissionCut cut;
    if (before_count) cut.last_before = timeline.last_before_prefix[before_count - 1];
    if (after_index < timeline.starts.size()) cut.first_after = timeline.first_after_suffix[after_index];
    if (cut.last_before && cut.first_after && *cut.last_before > *cut.first_after)
        throw std::runtime_error("Write insertion submission order disagrees with stream order");

    const auto first = std::ranges::lower_bound(timeline.unanchored, cut.last_before.value_or(0));
    const auto last = cut.first_after ? std::ranges::upper_bound(timeline.unanchored, *cut.first_after) : timeline.unanchored.end();
    cut.unanchored_ordinals = std::span(first, last);
    return cut;
}

} // namespace markov::trace_graph::modules::hicache::patch
