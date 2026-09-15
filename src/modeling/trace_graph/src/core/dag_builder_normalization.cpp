/**
 * @file
 * @brief Normalizes executable trace events before DAG node creation.
 */
#include "dag_builder_normalization.hpp"

#include <algorithm>
#include <numeric>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace markov::trace_graph::core {

namespace {

using dag_builder_detail::is_hicache_control_event;
using dag_builder_detail::node_end_ts;
using dag_builder_detail::resolve_event_lane;

class EventNormalizer {
public:
    explicit EventNormalizer(std::vector<TraceEvent> events) : events_(std::move(events)), dropped_(events_.size(), false) {}

    [[nodiscard]] std::vector<TraceEvent> run() {
        build_event_order();
        coalesce_runtime_boundaries();
        auto deduped = move_retained_events();
        sort_equal_timestamp_groups(deduped);
        return retain_cpu_leaves(std::move(deduped));
    }

private:
    struct LaneEvents {
        std::vector<size_t> indices;
        bool has_cpu = false;
    };

    struct LeafSelection {
        explicit LeafSelection(size_t event_count) : is_leaf(event_count, true), discarded(event_count, false), is_enqueue_node(event_count, false) {}

        std::vector<bool> is_leaf;
        std::vector<bool> discarded;
        std::vector<bool> is_enqueue_node;
    };

    [[nodiscard]] bool logical_index_less(size_t left, size_t right) const {
        const auto & lhs = events_[left];
        const auto & rhs = events_[right];
        if (lhs.ts != rhs.ts) return lhs.ts < rhs.ts;
        if (lhs.pid != rhs.pid) return lhs.pid < rhs.pid;
        if (lhs.tid != rhs.tid) return lhs.tid < rhs.tid;
        if (lhs.name != rhs.name) return lhs.name < rhs.name;
        if (lhs.index != rhs.index) return lhs.index < rhs.index;
        return left < right;
    }

    [[nodiscard]] size_t event_index_at(size_t position) const { return sorted_by_timestamp_ ? position : event_order_[position]; }

    void build_event_order() {
        sorted_by_timestamp_ = std::ranges::is_sorted(events_, [](const TraceEvent & left, const TraceEvent & right) { return left.ts < right.ts; });
        if (sorted_by_timestamp_) return;
        event_order_.resize(events_.size());
        std::iota(event_order_.begin(), event_order_.end(), size_t{ 0 });
        std::ranges::sort(event_order_, [this](size_t left, size_t right) { return logical_index_less(left, right); });
    }

    /**
     * @brief Coalesces torch and wrapper events that describe one runtime boundary.
     *
     * Match precise starts and compatible operation identities. Duration
     * differences are allowed only for explicitly identified views of the same
     * operation: HCCL views can round a hardware duration to a different precision.
     * Submission anchors are causal work, not duplicate device execution records.
     */
    void coalesce_runtime_boundaries() {
        for (size_t timestamp_begin = 0; timestamp_begin < events_.size();) {
            size_t timestamp_end = timestamp_begin + 1;
            const auto timestamp = events_[event_index_at(timestamp_begin)].ts;
            while (timestamp_end < events_.size() && events_[event_index_at(timestamp_end)].ts == timestamp) ++timestamp_end;
            if (timestamp_end - timestamp_begin > 1) coalesce_timestamp_group(timestamp_begin, timestamp_end);
            timestamp_begin = timestamp_end;
        }
    }

    void coalesce_timestamp_group(size_t begin, size_t end) {
        boundary_order_.clear();
        boundary_order_.reserve(end - begin);
        for (size_t position = begin; position < end; ++position) boundary_order_.push_back(event_index_at(position));
        std::ranges::sort(boundary_order_, [this](size_t left, size_t right) {
            const auto & lhs = events_[left];
            const auto & rhs = events_[right];
            if (lhs.ts_submicro_ns != rhs.ts_submicro_ns) return lhs.ts_submicro_ns < rhs.ts_submicro_ns;
            return logical_index_less(left, right);
        });

        for (size_t boundary_begin = 0; boundary_begin < boundary_order_.size();) {
            size_t boundary_end = boundary_begin + 1;
            const auto & first = events_[boundary_order_[boundary_begin]];
            while (boundary_end < boundary_order_.size()) {
                const auto & next = events_[boundary_order_[boundary_end]];
                if (first.ts_submicro_ns != next.ts_submicro_ns) break;
                ++boundary_end;
            }
            if (boundary_end - boundary_begin > 1) coalesce_boundary_group(boundary_begin, boundary_end);
            boundary_begin = boundary_end;
        }
    }

    static bool compatible_operation(const TraceEvent & left, const TraceEvent & right) {
        for (const auto key : { "connection_id", "Physic Stream Id", "Task Id", "Batch Id", "Subtask Id" }) {
            const auto lhs = left.find_arg(key);
            const auto rhs = right.find_arg(key);
            if (lhs && rhs && *lhs != *rhs) return false;
        }
        return true;
    }

    static bool shared_operation_identity(const TraceEvent & left, const TraceEvent & right) {
        const auto connection = left.arg("connection_id");
        if (!connection.empty() && connection == right.arg("connection_id")) return true;
        const auto stream = left.arg("Physic Stream Id", left.arg("stream id"));
        const auto task = left.arg("Task Id", left.arg("task id"));
        return !stream.empty() && !task.empty() && stream == right.arg("Physic Stream Id", right.arg("stream id"))
               && task == right.arg("Task Id", right.arg("task id"));
    }

    void coalesce_boundary_group(size_t begin, size_t end) {
        for (size_t device_position = begin; device_position < end; ++device_position) {
            const auto device_index = boundary_order_[device_position];
            if (dropped_[device_index] || is_hicache_control_event(events_[device_index]) || !events_[device_index].has_arg("Physic Stream Id")) continue;
            for (size_t position = begin; position < end; ++position) {
                const auto index = boundary_order_[position];
                if (index == device_index || dropped_[index] || is_hicache_control_event(events_[index])
                    || dag_builder_detail::is_submit_anchor_event(events_[index]))
                    continue;
                if (!compatible_operation(events_[device_index], events_[index])) continue;
                if (std::tie(events_[device_index].dur, events_[device_index].dur_submicro_ns) != std::tie(events_[index].dur, events_[index].dur_submicro_ns)
                    && !shared_operation_identity(events_[device_index], events_[index]))
                    continue;
                events_[device_index].merge_args_from(events_[index]);
                if (events_[index].tid == "0") events_[device_index].name = events_[index].name;
                dropped_[index] = true;
            }
        }
    }

    [[nodiscard]] std::vector<TraceEvent> move_retained_events() {
        std::vector<TraceEvent> retained;
        retained.reserve(events_.size());
        for (const auto position : std::views::iota(size_t{ 0 }, events_.size())) {
            const auto event_index = event_index_at(position);
            if (dropped_[event_index]) continue;
            retained.push_back(std::move(events_[event_index]));
        }
        return retained;
    }

    static void sort_equal_timestamp_groups(std::vector<TraceEvent> & events) {
        for (size_t begin = 0; begin < events.size();) {
            size_t end = begin + 1;
            while (end < events.size() && events[end].ts == events[begin].ts) ++end;
            if (end - begin > 1) {
                std::sort(events.begin() + static_cast<std::ptrdiff_t>(begin),
                          events.begin() + static_cast<std::ptrdiff_t>(end),
                          [](const TraceEvent & left, const TraceEvent & right) {
                              if (left.dur != right.dur) return left.dur > right.dur;
                              if (left.pid != right.pid) return left.pid < right.pid;
                              if (left.tid != right.tid) return left.tid < right.tid;
                              if (left.name != right.name) return left.name < right.name;
                              return left.index < right.index;
                          });
            }
            begin = end;
        }
    }

    /**
     * @brief Builds per-lane sequences and retains only leaves from nested CPU call trees.
     *
     * Device events never enter CPU-leaf filtering because kernels and copies are
     * executable graph nodes rather than nested CPU call frames. Semantic HiCache
     * facts are separated before normalization and do not enter this executable path.
     */
    [[nodiscard]] static std::vector<TraceEvent> retain_cpu_leaves(std::vector<TraceEvent> events) {
        auto lanes = build_lane_index(events);
        LeafSelection selection(events.size());
        std::unordered_set<std::string> seen_correlation_ids;
        for (auto & [lane, lane_events] : lanes) {
            (void)lane;
            if (!lane_events.has_cpu) continue;
            mark_lane_leaves(events, lane_events.indices, selection, seen_correlation_ids);
        }

        std::vector<TraceEvent> result;
        result.reserve(events.size());
        for (const auto index : std::views::iota(size_t{ 0 }, events.size())) {
            if (is_hicache_control_event(events[index])) selection.is_leaf[index] = false;
        }
        append_hicache_control_self_time(events, lanes, selection, result);
        for (const auto index : std::views::iota(size_t{ 0 }, events.size())) {
            if (selection.is_leaf[index] && !selection.discarded[index]) result.push_back(std::move(events[index]));
        }
        std::ranges::stable_sort(result, [](const TraceEvent & left, const TraceEvent & right) {
            if (left.ts != right.ts) return left.ts < right.ts;
            const bool left_control_self = left.name.ends_with(".self");
            const bool right_control_self = right.name.ends_with(".self");
            if (left_control_self != right_control_self) return !left_control_self;
            if (left.dur != right.dur) return left.dur > right.dur;
            if (left.pid != right.pid) return left.pid < right.pid;
            if (left.tid != right.tid) return left.tid < right.tid;
            return left.index < right.index;
        });
        for (const auto index : std::views::iota(size_t{ 0 }, result.size())) result[index].index = index;
        return result;
    }

    static void append_hicache_control_self_time(const std::vector<TraceEvent> & events, const auto & lanes, const LeafSelection & selection,
                                                 std::vector<TraceEvent> & output) {
        for (const auto & [lane, lane_events] : lanes) {
            if (lane_events.has_cpu == false) continue;
            std::vector<size_t> retained_leaves;
            retained_leaves.reserve(lane_events.indices.size());
            for (const auto event_index : lane_events.indices) {
                if (selection.is_leaf[event_index] && !selection.discarded[event_index]) retained_leaves.push_back(event_index);
            }
            for (const auto marker_index : lane_events.indices) {
                const auto & marker = events[marker_index];
                if (!is_hicache_control_event(marker)) continue;
                const auto marker_end = node_end_ts(marker);
                if (marker_end <= marker.ts) continue;

                std::vector<std::pair<uint64_t, uint64_t>> covered;
                auto first_leaf = std::ranges::lower_bound(retained_leaves, marker.ts, {}, [&](size_t event_index) { return events[event_index].ts; });
                if (first_leaf != retained_leaves.begin()) {
                    const auto & previous_leaf = events[*std::prev(first_leaf)];
                    const auto covered_end = std::min(node_end_ts(previous_leaf), marker_end);
                    if (covered_end > marker.ts) covered.emplace_back(marker.ts, covered_end);
                }
                for (auto leaf_it = first_leaf; leaf_it != retained_leaves.end(); ++leaf_it) {
                    const auto & leaf = events[*leaf_it];
                    if (leaf.ts >= marker_end) break;
                    const auto covered_start = std::max(leaf.ts, marker.ts);
                    const auto covered_end = std::min(node_end_ts(leaf), marker_end);
                    if (covered_end >= covered_start) covered.emplace_back(covered_start, covered_end);
                }
                const auto first_control =
                    std::ranges::lower_bound(lane_events.indices, marker.ts, {}, [&](size_t event_index) { return events[event_index].ts; });
                for (auto control_it = first_control; control_it != lane_events.indices.end(); ++control_it) {
                    const auto child_index = *control_it;
                    if (child_index == marker_index) continue;
                    const auto & child = events[child_index];
                    if (child.ts >= marker_end) break;
                    if (!is_hicache_control_event(child) || child.dur == 0) continue;
                    const auto child_end = node_end_ts(child);
                    const bool nested_control =
                        child.ts >= marker.ts && child_end <= marker_end && (child.ts > marker.ts || child_end < marker_end || child.index > marker.index);
                    if (nested_control) covered.emplace_back(child.ts, child_end);
                }
                std::ranges::sort(covered);
                uint64_t cursor = marker.ts;
                for (const auto & [covered_start, covered_end] : covered) {
                    if (covered_end <= cursor) continue;
                    if (covered_start > cursor) { append_hicache_control_self_event(marker, cursor, std::min(covered_start, marker_end), output); }
                    cursor = std::max(cursor, covered_end);
                    if (cursor >= marker_end) break;
                }
                if (cursor < marker_end) { append_hicache_control_self_event(marker, cursor, marker_end, output); }
            }
        }
    }

    static void append_hicache_control_self_event(const TraceEvent & marker, uint64_t start, uint64_t end, std::vector<TraceEvent> & output) {
        if (end <= start) return;
        TraceEvent self;
        self.source_channel = marker.source_channel;
        self.name = marker.name + ".self";
        self.cat = "cpu_op";
        self.ph = 'X';
        self.ts = start;
        self.dur = end - start;
        self.pid = marker.pid;
        self.tid = marker.tid;
        self.set_arg("hicache_control_parent", marker.name);
        self.set_arg("hicache_control_parent_index", std::to_string(marker.index));
        self.set_arg("hicache_control_semantics", "parent_self_time");
        output.push_back(std::move(self));
    }

    [[nodiscard]] static std::unordered_map<std::string, LaneEvents> build_lane_index(const std::vector<TraceEvent> & events) {
        std::unordered_map<std::string, LaneEvents> lanes;
        for (const auto index : std::views::iota(size_t{ 0 }, events.size())) {
            auto identity = resolve_event_lane(events[index], false);
            auto & lane = lanes[std::move(identity.lane)];
            lane.indices.push_back(index);
            lane.has_cpu = lane.has_cpu || !identity.is_device;
        }
        return lanes;
    }

    [[nodiscard]] static std::string correlation_id(const TraceEvent & event) {
        return event.has_arg_key_hint("correlation_id") ? event.arg("correlation_id") : std::string{};
    }

    using SubmicroTime = unsigned __int128;

    [[nodiscard]] static SubmicroTime submicro_timestamp(const TraceEvent & event) {
        return static_cast<SubmicroTime>(event.ts) * 1'000 + event.ts_submicro_ns;
    }

    [[nodiscard]] static SubmicroTime submicro_duration(const TraceEvent & event) {
        return static_cast<SubmicroTime>(event.dur) * 1'000 + event.dur_submicro_ns;
    }

    static void resolve_nested_parent(std::vector<TraceEvent> & events, size_t current_index, std::vector<size_t> & stack, LeafSelection & selection,
                                      const std::string & current_correlation_id) {
        auto & current = events[current_index];
        while (!stack.empty()) {
            const auto parent_index = stack.back();
            auto & parent = events[parent_index];
            // Compare twice the endpoints in integer nanoseconds. The DAG remains at
            // microsecond granularity, but leaf selection must not turn a contained child
            // into a sibling merely because start and duration were truncated separately.
            const auto current_end_threshold_twice = submicro_timestamp(current) * 2 + submicro_duration(current);
            const auto parent_end_twice = (submicro_timestamp(parent) + submicro_duration(parent)) * 2;
            if (parent_end_twice <= current_end_threshold_twice) {
                stack.pop_back();
                continue;
            }
            if (parent.name == "Node@launch" || selection.is_enqueue_node[parent_index] || current.name.starts_with("Runtime@")) {
                selection.discarded[current_index] = true;
                return;
            }
            if (parent.name != "AscendCL@aclrtRecordEvent") selection.is_leaf[parent_index] = false;
            if (current_correlation_id.empty()) {
                const auto parent_correlation_id = correlation_id(parent);
                if (!parent_correlation_id.empty()) current.set_arg("correlation_id", parent_correlation_id);
            }
            return;
        }
    }

    /**
     * @brief Applies the nested CPU-frame heuristic to one timestamp-ordered lane.
     *
     * `Node@launch`, first-correlation enqueue nodes, runtime implementation details,
     * and `AscendCL@aclrtRecordEvent` retain the historical faithful-replay exceptions.
     */
    static void mark_lane_leaves(std::vector<TraceEvent> & events, const std::vector<size_t> & indices, LeafSelection & selection,
                                 std::unordered_set<std::string> & seen_correlation_ids) {
#ifdef DEBUG
        if (!std::ranges::is_sorted(indices, [&](size_t left, size_t right) {
                if (events[left].ts != events[right].ts) return events[left].ts < events[right].ts;
                return left < right;
            })) {
            throw std::logic_error("normalized lane order must preserve global timestamp order");
        }
#endif

        std::vector<size_t> stack;
        for (const size_t current_index : indices) {
            auto & current = events[current_index];
            if (is_hicache_control_event(current)) continue;
            const auto current_correlation_id = correlation_id(current);
            if (!current_correlation_id.empty() && seen_correlation_ids.insert(current_correlation_id).second) selection.is_enqueue_node[current_index] = true;
            resolve_nested_parent(events, current_index, stack, selection, current_correlation_id);
            if (!selection.discarded[current_index]) stack.push_back(current_index);
        }
    }

    std::vector<TraceEvent> events_;
    std::vector<bool> dropped_;
    std::vector<size_t> event_order_;
    std::vector<size_t> boundary_order_;
    bool sorted_by_timestamp_ = true;
};

} // namespace

std::vector<TraceEvent> normalize_events(std::vector<TraceEvent> events) { return EventNormalizer(std::move(events)).run(); }

} // namespace markov::trace_graph::core
