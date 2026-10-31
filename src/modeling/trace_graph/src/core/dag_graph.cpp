/**
 * @file
 * @brief Compact DAG storage, cross-rank merge, and summary statistics.
 */
#include "markov/trace_graph/core/dag_graph.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <numeric>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::core {

/**
 * @brief Returns the stable user-visible name for an edge kind.
 *
 * Names are lowercase output values consumed by run summaries and diagnostics.
 */
std::string edge_kind_name(DagEdgeKind kind) {
    switch (kind) {
    case DagEdgeKind::Sequential:
        return "sequential";
    case DagEdgeKind::Stream:
        return "stream";
    case DagEdgeKind::Correlation:
        return "correlation";
    case DagEdgeKind::Sync:
        return "sync";
    case DagEdgeKind::HCCL:
        return "hccl";
    case DagEdgeKind::HiCache:
        return "hicache";
    case DagEdgeKind::Mutation:
        return "mutation";
    }
    return "unknown";
}

DagEdge::DagEdge(size_t src_value, size_t dst_value, DagEdgeKind kind_value, std::string_view effect_id_value, std::string_view reason_value)
    : src(src_value),
      dst(dst_value),
      kind(kind_value) {
    if (!effect_id_value.empty() || !reason_value.empty()) {
        provenance = std::make_unique<DagEdgeProvenance>(DagEdgeProvenance{
            .effect_id = std::string(effect_id_value),
            .reason = std::string(reason_value),
        });
    }
}

DagEdge::DagEdge(const DagEdge & other) : src(other.src), dst(other.dst), kind(other.kind), active(other.active) {
    if (other.provenance) provenance = std::make_unique<DagEdgeProvenance>(*other.provenance);
}

DagEdge & DagEdge::operator=(const DagEdge & other) {
    if (this == &other) return *this;
    src = other.src;
    dst = other.dst;
    kind = other.kind;
    active = other.active;
    provenance = other.provenance ? std::make_unique<DagEdgeProvenance>(*other.provenance) : nullptr;
    return *this;
}

std::string_view DagEdge::effect_id() const { return provenance ? std::string_view(provenance->effect_id) : std::string_view{}; }

std::string_view DagEdge::reason() const { return provenance ? std::string_view(provenance->reason) : std::string_view{}; }

DagGraph::DagGraph(std::vector<TraceEvent> events, int gpu_id) : events_(std::move(events)), gpu_id_(gpu_id) {}

void DagGraph::reserve(const DagGraphCapacity & capacity) {
    nodes_.reserve(capacity.nodes);
    edges_.reserve(capacity.edges);
    scope_node_owned_.reserve(capacity.nodes);
    scope_gap_durations_.reserve(capacity.nodes);
}

size_t DagGraph::intern_lane(std::string_view lane_key_value) {
    if (const auto found = lane_ids_.find(lane_key_value); found != lane_ids_.end()) return found->second;

    const auto lane_id = lane_keys_.size();
    lane_keys_.emplace_back(lane_key_value);
    try {
        lane_ids_.emplace(lane_keys_.back(), lane_id);
    }
    catch (...) {
        lane_keys_.pop_back();
        throw;
    }
    return lane_id;
}

size_t DagGraph::add_node(size_t event_index, bool is_cpu, std::string_view lane_key_value) {
    if (event_index >= event_count()) { throw std::out_of_range("event index out of range while adding DAG node"); }
    invalidate_topology();
    cpu_intervals_.reset();
    size_t node_id = nodes_.size();
    const auto & event = this->event(event_index);
    DagNode node;
    node.id = node_id;
    node.event_index = event_index;
    node.gpu_id = gpu_id_;
    node.is_cpu = is_cpu;
    node.lane_id = intern_lane(lane_key_value);
    node.duration = event.dur;
    node.original_duration = event.dur;
    node.counts_toward_e2e = event.arg("counts_toward_e2e") != "false";
    nodes_.push_back(node);
    scope_node_owned_.push_back(0);
    scope_gap_durations_.push_back(0);
    return node_id;
}

size_t DagGraph::add_device_resource(std::string lane_key, int gpu_id) {
    const auto node = add_synthetic_node({ .name = "device resource", .is_cpu = false, .lane_key = std::move(lane_key) });
    disable_node(node);
    nodes_[node].gpu_id = gpu_id;
    return node;
}

size_t DagGraph::add_synthetic_node(DagSyntheticNodeSpec spec) {
    if (spec.name.empty()) throw std::invalid_argument("synthetic DAG node name must not be empty");
    if (spec.cpu_task_ready_delay_us && !spec.is_cpu) throw std::invalid_argument("explicit CPU task must use a CPU node");
    if (spec.cpu_gap_after && (!spec.is_cpu || spec.cpu_task_ready_delay_us)) throw std::invalid_argument("residual gap requires a non-worker CPU boundary");
    TraceEvent event;
    event.index = event_count();
    event.source_channel = TraceSourceChannel::Synthetic;
    event.event_id = "synthetic_" + std::to_string(nodes_.size());
    event.name = std::move(spec.name);
    event.cat = std::move(spec.category);
    event.ph = 'X';
    event.dur = spec.duration;
    event.pid = std::to_string(gpu_id_);
    event.tid = spec.lane_key;
    if (spec.observed_point) {
        event.pid = spec.observed_point->pid;
        event.tid = spec.observed_point->tid;
        event.ts = spec.observed_point->timestamp_us;
    }
    if (spec.shared_attrs) event.share_args(std::move(spec.shared_attrs));
    event.set_arg_overrides(std::move(spec.attrs));
    events_.push_back(std::move(event));

    const auto node_id = add_node(event_count() - 1, spec.is_cpu, spec.lane_key);
    auto & node = nodes_[node_id];
    node.kind = DagNodeKind::Synthetic;
    node.counts_toward_e2e = spec.counts_toward_e2e;
    node.explicit_cpu_task = spec.cpu_task_ready_delay_us.has_value();
    node.cpu_ready_delay_before = spec.cpu_task_ready_delay_us.value_or(0);
    node.cpu_gap_after = spec.cpu_gap_after;
    if (spec.observed_point) node.gpu_id = spec.observed_point->gpu_id;
    return node_id;
}

size_t DagGraph::add_edge(size_t src, size_t dst, DagEdgeKind kind, std::string_view effect_id, std::string_view reason) {
    if (src >= nodes_.size() || dst >= nodes_.size()) { throw std::out_of_range("DAG edge endpoint is out of range"); }
    /**
     * @brief Deliberately leaves duplicate-edge prevention to callers.
     *
     * Duplicate dependencies preserve the critical path but inflate indegrees and
     * summary counts, so each edge-producing rule must own deduplication.
     */
    invalidate_topology();
    const auto edge_index = edges_.size();
    edges_.emplace_back(src, dst, kind, effect_id, reason);
    if (topological_order_) edited_edges_.push_back(edge_index);
    return edge_index;
}

void DagGraph::disable_edge(size_t edge_index) {
    auto & edge = edges_.at(edge_index);
    if (!edge.active) return;
    invalidate_topology();
    edge.active = false;
}

void DagGraph::disable_node(size_t node_id) {
    auto & target = mutable_node(node_id);
    if (!target.active) return;
    target.active = false;
    release_topological_order();
    invalidate_topology();
    cpu_intervals_.reset();
}

const DagEdge & DagGraph::edge(size_t edge_index) const {
    if (edge_index >= edges_.size()) throw std::out_of_range("DAG edge index out of range");
    return edges_[edge_index];
}

void DagGraph::redirect_edge_target(size_t edge_index, size_t target) {
    if (target >= node_count()) throw std::out_of_range("DAG edge target is out of range");
    auto & edge = edges_.at(edge_index);
    invalidate_topology();
    edge.dst = target;
    if (topological_order_) edited_edges_.push_back(edge_index);
}

void DagGraph::share_source_events() {
    if (source_events_) return;
    source_events_ = std::make_shared<const std::vector<TraceEvent>>(std::move(events_));
    events_.clear();
}

void DagGraph::reserve_events(size_t capacity) {
    const auto shared = source_events_ ? source_events_->size() : 0;
    if (capacity > shared) events_.reserve(capacity - shared);
}

void DagGraph::materialize_events() {
    if (!source_events_) return;
    std::vector<TraceEvent> combined;
    combined.reserve(event_count());
    for (size_t i = 0; i < source_events_->size(); ++i) combined.push_back(event(i));
    combined.insert(combined.end(), std::make_move_iterator(events_.begin()), std::make_move_iterator(events_.end()));
    events_ = std::move(combined);
    event_overrides_.clear();
    source_events_.reset();
}

DagGraph::ObservedTables & DagGraph::own_observations() {
    if (!observations_.unique()) observations_ = std::make_shared<ObservedTables>(*observations_);
    return *observations_;
}

void DagGraph::invalidate_topology() {
    adjacency_.reset();
    active_counts_.reset();
}

void DagGraph::release_topological_order() {
    topological_order_.reset();
    edited_edges_.clear();
}

const TraceEvent & DagGraph::event(size_t event_index) const {
    const auto shared = source_events_ ? source_events_->size() : 0;
    if (event_index < shared) {
        if (!event_overrides_.empty())
            if (const auto found = event_overrides_.find(event_index); found != event_overrides_.end()) return found->second;
        return (*source_events_)[event_index];
    }
    return events_.at(event_index - shared);
}

TraceEvent & DagGraph::mutable_event(size_t event_index) {
    cpu_intervals_.reset();
    if (cpu_task_observations_ && event_index < cpu_task_observations_->event_count) cpu_task_observations_.reset();
    const auto shared = source_events_ ? source_events_->size() : 0;
    if (event_index < shared) {
        const auto found = event_overrides_.find(event_index);
        if (found != event_overrides_.end()) return found->second;
        return event_overrides_.emplace(event_index, (*source_events_)[event_index]).first->second;
    }
    return events_.at(event_index - shared);
}

const TraceEvent & DagGraph::event_for_node(size_t node_id) const { return event(node(node_id).event_index); }

TraceEvent & DagGraph::mutable_event_for_node(size_t node_id) { return mutable_event(node(node_id).event_index); }

const DagNode & DagGraph::node(size_t node_id) const {
    if (node_id >= nodes_.size()) { throw std::out_of_range("DAG node id out of range"); }
    return nodes_[node_id];
}

DagNode & DagGraph::mutable_node(size_t node_id) {
    if (node_id >= nodes_.size()) { throw std::out_of_range("DAG node id out of range"); }
    return nodes_[node_id];
}

void DagGraph::set_node_duration(size_t node_id, uint64_t duration) { mutable_node(node_id).duration = duration; }

void DagGraph::bind_node_execution_duration(size_t node_id, uint64_t duration) {
    auto & target = mutable_node(node_id);
    target.duration = duration;
    target.execution_cost_bound = true;
}

uint64_t DagGraph::cpu_service_node_duration(size_t id) const {
    const auto & n = node(id);
    if (n.execution_cost_bound || cpu_service_cost_.empty() || !n.is_cpu || (n.kind == DagNodeKind::Synthetic && !n.observed_cpu_coordinates)
        || n.duration == 0)
        return n.duration;
    const auto & e = event_for_node(id);
    const auto service = cpu_service_cost_.duration({ e.pid, e.tid }, e.ts, checked_add_u64(e.ts, e.dur, "CPU service event end overflow"));
    if (service == e.dur) return n.duration;
    if (n.duration != e.dur)
        throw std::runtime_error("CPU service overlaps an already remodeled node: node=" + std::to_string(id) + " event=" + e.name
                                 + " duration_us=" + std::to_string(n.duration) + " observed_us=" + std::to_string(e.dur)
                                 + " corrected_us=" + std::to_string(service) + " timestamp_us=" + std::to_string(e.ts));
    return service;
}

uint64_t DagGraph::cpu_service_ready_delay(size_t id, uint64_t observed_delay_us) const {
    const auto & n = node(id);
    if (cpu_service_cost_.empty() || !n.is_cpu || observed_delay_us == 0 || (n.kind == DagNodeKind::Synthetic && !n.observed_cpu_coordinates))
        return observed_delay_us;
    const auto & event = event_for_node(id);
    return cpu_service_cost_.duration({ event.pid, event.tid }, event.ts - observed_delay_us, event.ts);
}

uint64_t DagGraph::cpu_service_gap_duration(size_t id) const {
    const auto & n = node(id);
    if (cpu_service_cost_.empty() || !n.is_cpu || (n.kind == DagNodeKind::Synthetic && !n.observed_cpu_coordinates) || n.cpu_gap_after == 0)
        return n.cpu_gap_after;
    const auto & e = event_for_node(id);
    const auto begin = checked_add_u64(e.ts, e.dur, "CPU service gap begin overflow");
    const auto end = checked_add_u64(begin, n.original_cpu_gap_after, "CPU service gap end overflow");
    if (const auto found = retained_cpu_gap_ranges_.find(id); found != retained_cpu_gap_ranges_.end()) {
        validate_cpu_gap_ranges(id, n.cpu_gap_after, found->second);
        uint64_t service = 0;
        for (const auto & [lo, hi] : found->second)
            service = checked_add_u64(service, cpu_service_cost_.duration({ e.pid, e.tid }, lo, hi), "Retained gap service overflow");
        return service;
    }
    const auto service = cpu_service_cost_.duration({ e.pid, e.tid }, begin, end);
    if (service == n.original_cpu_gap_after) return n.cpu_gap_after;
    if (n.cpu_gap_after != n.original_cpu_gap_after)
        throw std::runtime_error("CPU service overlaps a partially remodeled gap without retained coordinates: node=" + std::to_string(id) + " event=" + e.name
                                 + " gap=" + std::to_string(n.cpu_gap_after) + " original=" + std::to_string(n.original_cpu_gap_after)
                                 + " begin=" + std::to_string(begin));
    return service;
}

void DagGraph::set_node_counts_toward_e2e(size_t node_id, bool value) { mutable_node(node_id).counts_toward_e2e = value; }

void DagGraph::validate_cpu_gap_ranges(size_t node_id, uint64_t duration, const CpuGapRanges & ranges) const {
    const auto & n = node(node_id);
    if (!n.is_cpu || (n.kind == DagNodeKind::Synthetic && !n.observed_cpu_coordinates))
        throw std::invalid_argument("Retained gap ranges require original CPU coordinates");
    const auto & event = event_for_node(node_id);
    const auto begin = checked_add_u64(event.ts, event.dur, "Retained gap begin overflow");
    const auto end = checked_add_u64(begin, n.original_cpu_gap_after, "Retained gap end overflow");
    uint64_t cursor = begin, total = 0;
    for (const auto & [lo, hi] : ranges) {
        if (lo < cursor || hi <= lo || hi > end) throw std::invalid_argument("Retained gap ranges overlap or exceed their source");
        total = checked_add_u64(total, hi - lo, "Retained gap length overflow");
        cursor = hi;
    }
    if (total != duration) throw std::invalid_argument("Retained gap ranges do not account for the remaining duration");
}

void DagGraph::set_cpu_gap_after(size_t node_id, uint64_t duration, std::optional<CpuGapRanges> ranges) {
    auto & target = mutable_node(node_id);
    if (!target.is_cpu) throw std::invalid_argument("CPU gap update requires a CPU node");
    if (ranges) {
        validate_cpu_gap_ranges(node_id, duration, *ranges);
        retained_cpu_gap_ranges_[node_id] = std::move(*ranges);
    }
    else retained_cpu_gap_ranges_.erase(node_id);
    target.cpu_gap_after = duration;
}

std::optional<DagGraph::CpuGapRanges> DagGraph::observed_cpu_gap_ranges(size_t id) const {
    const auto & n = node(id);
    if (!n.cpu_gap_after) return CpuGapRanges{};
    if (const auto found = retained_cpu_gap_ranges_.find(id); found != retained_cpu_gap_ranges_.end()) return found->second;
    if (!n.is_cpu || (n.kind == DagNodeKind::Synthetic && !n.observed_cpu_coordinates) || n.cpu_gap_after != n.original_cpu_gap_after) return std::nullopt;

    const auto & event = event_for_node(id);
    const auto begin = checked_add_u64(event.ts, event.dur, "Observed CPU gap begin overflow");
    return CpuGapRanges{
        { begin, checked_add_u64(begin, n.cpu_gap_after, "Observed CPU gap end overflow") }
    };
}

void DagGraph::clear_scope_ownership() {
    scope_node_owned_.assign(nodes_.size(), 0);
    scope_gap_durations_.assign(nodes_.size(), 0);
}

void DagGraph::clear_scope_gap_ownership() { scope_gap_durations_.assign(nodes_.size(), 0); }

void DagGraph::set_scope_node_owned(size_t node_id) {
    if (node_id >= nodes_.size()) throw std::out_of_range("scope owner node ID out of range");
    scope_node_owned_[node_id] = 1;
}

void DagGraph::add_scope_gap_duration(size_t node_id, uint64_t duration) {
    if (node_id >= nodes_.size()) throw std::out_of_range("scope gap owner node ID out of range");
    scope_gap_durations_[node_id] = checked_add_u64(scope_gap_durations_[node_id], duration, "scope-owned CPU gap exceeds uint64 range");
    if (scope_gap_durations_[node_id] > nodes_[node_id].cpu_gap_after) throw std::logic_error("scope-owned CPU gap exceeds materialized CPU gap");
}

bool DagGraph::scope_node_owned(size_t node_id) const {
    if (node_id >= nodes_.size()) throw std::out_of_range("scope owner node ID out of range");
    return scope_node_owned_[node_id] != 0;
}

uint64_t DagGraph::scope_gap_duration(size_t node_id) const {
    if (node_id >= nodes_.size()) throw std::out_of_range("scope gap owner node ID out of range");
    return scope_gap_durations_[node_id];
}

std::string_view DagGraph::node_lane_key(size_t node_id) const { return lane_key(node(node_id).lane_id); }

std::string_view DagGraph::lane_key(size_t lane_id) const {
    if (lane_id >= lane_keys_.size()) throw std::out_of_range("DAG lane id out of range");
    return lane_keys_[lane_id];
}

std::optional<size_t> DagGraph::find_lane_id(std::string_view lane_key_value) const {
    const auto found = lane_ids_.find(lane_key_value);
    if (found == lane_ids_.end()) return std::nullopt;
    return found->second;
}

size_t DagGraph::active_node_count() const { return active_counts().nodes; }

size_t DagGraph::active_edge_count() const { return active_counts().edges; }

bool DagGraph::has_active_edge(size_t src, size_t dst, DagEdgeKind kind, std::string_view effect_id) const {
    return std::ranges::any_of(edges_, [&](const auto & edge) {
        return edge.active && edge.src == src && edge.dst == dst && edge.kind == kind && edge.effect_id() == effect_id;
    });
}

void DagGraph::set_input_contracts(std::vector<std::string> contracts) {
    std::ranges::sort(contracts);
    contracts.erase(std::unique(contracts.begin(), contracts.end()), contracts.end());
    input_contracts_ = std::move(contracts);
}

bool DagGraph::has_input_contract(std::string_view contract) const { return std::ranges::binary_search(input_contracts_, contract); }

DagGraphSummaryStats DagGraph::summary_stats() const {
    DagGraphSummaryStats stats;
    for (const auto & node : nodes_) {
        if (!node.active) continue;
        stats.active_node_count++;
        if (node.kind == DagNodeKind::Synthetic) stats.active_synthetic_node_count++;
        else stats.active_trace_node_count++;
    }
    for (const auto & edge : edges_) {
        if (!edge.active || edge.src >= nodes_.size() || edge.dst >= nodes_.size() || !nodes_[edge.src].active || !nodes_[edge.dst].active) continue;
        stats.active_edge_count++;
        stats.edge_counts_by_kind[edge_kind_name(edge.kind)]++;
    }
    return stats;
}

const DagGraph::ActiveCounts & DagGraph::active_counts() const {
    if (!active_counts_) {
        const auto nodes = static_cast<size_t>(std::ranges::count_if(nodes_, [](const auto & node) { return node.active; }));
        const auto edges = static_cast<size_t>(std::ranges::count_if(edges_, [&](const auto & edge) {
            return edge.active && edge.src < nodes_.size() && edge.dst < nodes_.size() && nodes_[edge.src].active && nodes_[edge.dst].active;
        }));
        active_counts_ = ActiveCounts{ nodes, edges };
    }
    return *active_counts_;
}

class DagGraph::GraphMerger {
public:
    explicit GraphMerger(std::vector<DagGraph> graphs) : graphs_(std::move(graphs)), merged_({}, 0), node_offsets_(graphs_.size(), 0) {}

    [[nodiscard]] DagGraph run() {
        for (auto & graph : graphs_) {
            graph.materialize_events();
            graph.own_observations();
        }
        reserve_node_storage();
        merge_nodes_and_events();
        reserve_and_merge_edges();
        align_hccl_groups();
        return std::move(merged_);
    }

private:
    using HcclRanks = std::unordered_map<size_t, std::vector<size_t>>;
    using HcclGroups = std::unordered_map<std::string, HcclRanks>;
    using AlignedHcclNodes = std::vector<std::pair<size_t, size_t>>;

    /** @brief Stable offsets used while relocating one source graph. */
    struct GraphRelocation {
        size_t graph_index = 0;
        size_t event_offset = 0;
        size_t node_offset = 0;
        size_t source_node_count = 0;
    };

    template <typename SizeOf> [[nodiscard]] size_t checked_total(SizeOf size_of, std::string_view label) const {
        size_t total = 0;
        for (const auto & graph : graphs_) {
            const auto value = size_of(graph);
            if (value > std::numeric_limits<size_t>::max() - total) { throw std::length_error("DAG merge " + std::string(label) + " count overflow"); }
            total += value;
        }
        return total;
    }

    void reserve_node_storage() {
        total_nodes_ = checked_total([](const auto & graph) { return graph.nodes_.size(); }, "node");
        total_edges_ = checked_total([](const auto & graph) { return graph.edges_.size(); }, "edge");
        merged_.events_.reserve(checked_total([](const auto & graph) { return graph.events_.size(); }, "event"));
        merged_.parsed_record_count_ = checked_total([](const auto & graph) { return graph.parsed_record_count_; }, "parsed-record");
        merged_.observations_->hicache_fact_events.reserve(
            checked_total([](const auto & graph) { return graph.observations_->hicache_fact_events.size(); }, "HiCache fact"));
        merged_.observations_->runtime_observations.reserve(
            checked_total([](const auto & graph) { return graph.observations_->runtime_observations.size(); }, "runtime observation"));
        merged_.observations_->context_events.reserve(
            checked_total([](const auto & graph) { return graph.observations_->context_events.size(); }, "context event"));
        merged_.observations_->prelude_context_events.reserve(
            checked_total([](const auto & graph) { return graph.observations_->prelude_context_events.size(); }, "prelude context event"));
        merged_.observations_->tail_context_events.reserve(
            checked_total([](const auto & graph) { return graph.observations_->tail_context_events.size(); }, "tail context event"));
        merged_.observations_->control_exclusion_intervals.reserve(
            checked_total([](const auto & graph) { return graph.observations_->control_exclusion_intervals.size(); }, "control exclusion interval"));
        merged_.observations_->phase_marker_events.reserve(
            checked_total([](const auto & graph) { return graph.observations_->phase_marker_events.size(); }, "phase marker event"));
        merged_.reserve(DagGraphCapacity{ .nodes = total_nodes_, .edges = 0 });
    }

    void merge_nodes_and_events() {
        size_t event_offset = 0;
        size_t node_offset = 0;
        for (size_t graph_index = 0; graph_index < graphs_.size(); ++graph_index) {
            auto & graph = graphs_[graph_index];
            node_offsets_[graph_index] = node_offset;
            merged_.input_contracts_.insert(merged_.input_contracts_.end(), graph.input_contracts_.begin(), graph.input_contracts_.end());
            merged_.observations_->runtime_observations.insert(merged_.observations_->runtime_observations.end(),
                                                               std::make_move_iterator(graph.observations_->runtime_observations.begin()),
                                                               std::make_move_iterator(graph.observations_->runtime_observations.end()));
            append_indexed_events(merged_.observations_->hicache_fact_events, graph.observations_->hicache_fact_events);
            merged_.observations_->context_events.insert(merged_.observations_->context_events.end(),
                                                         std::make_move_iterator(graph.observations_->context_events.begin()),
                                                         std::make_move_iterator(graph.observations_->context_events.end()));
            append_indexed_events(merged_.observations_->prelude_context_events, graph.observations_->prelude_context_events);
            append_indexed_events(merged_.observations_->tail_context_events, graph.observations_->tail_context_events);
            merged_.observations_->control_exclusion_intervals.insert(merged_.observations_->control_exclusion_intervals.end(),
                                                                      std::make_move_iterator(graph.observations_->control_exclusion_intervals.begin()),
                                                                      std::make_move_iterator(graph.observations_->control_exclusion_intervals.end()));
            merged_.observations_->phase_marker_events.insert(merged_.observations_->phase_marker_events.end(),
                                                              std::make_move_iterator(graph.observations_->phase_marker_events.begin()),
                                                              std::make_move_iterator(graph.observations_->phase_marker_events.end()));
            remap_nodes(graph,
                        GraphRelocation{
                            .graph_index = graph_index,
                            .event_offset = event_offset,
                            .node_offset = node_offset,
                            .source_node_count = graph.nodes_.size(),
                        });
            append_nodes_and_events(graph);
            event_offset += graph.events_.size();
            node_offset += graph.nodes_.size();
        }
        merged_.set_input_contracts(std::move(merged_.input_contracts_));
#ifdef DEBUG
        merged_.real_e2e_time_ = has_real_time_ && real_max_ > real_min_ ? real_max_ - real_min_ : 0;
#endif
    }

    // Each owning event table has its own index space. Keep source vector sizes
    // intact until node/event offsets have been advanced for this rank.
    static void append_indexed_events(std::vector<TraceEvent> & destination, std::vector<TraceEvent> & source) {
        for (auto & event : source) {
            event.index = destination.size();
            destination.push_back(std::move(event));
        }
    }

    void remap_nodes(DagGraph & graph, const GraphRelocation & relocation) {
        for (auto & node : graph.nodes_) {
            const auto local_node_id = node.id;
            const auto source_lane = graph.lane_key(node.lane_id);
            inspect_trace_node(graph, relocation, local_node_id, node);
            node.id += relocation.node_offset;
            node.event_index += relocation.event_offset;
            node.lane_id = merged_.intern_lane(source_lane);
            remap_hccl_successor(node, relocation);
        }
    }

    void inspect_trace_node(const DagGraph & graph, const GraphRelocation & relocation, size_t local_node_id, const DagNode & node) {
        if (!node.active || node.kind == DagNodeKind::Synthetic) return;
        const auto & event = graph.event_for_node(local_node_id);
        if (!node.is_cpu && event.collective_envelope) hccl_groups_[event.name][relocation.graph_index].push_back(local_node_id + relocation.node_offset);
#ifdef DEBUG
        if (!has_real_time_ || event.ts < real_min_) real_min_ = event.ts;
        real_max_ = std::max(real_max_, checked_add_u64(event.ts, event.dur, "trace timestamp overflow while merging DAGs"));
        has_real_time_ = true;
#endif
    }

    static void remap_hccl_successor(DagNode & node, const GraphRelocation & relocation) {
        if (node.hccl_successor_node_id == DagNode::kNoNode) return;
        if (node.hccl_successor_node_id >= relocation.source_node_count) throw std::runtime_error("HCCL successor node is out of range before DAG merge");
        node.hccl_successor_node_id += relocation.node_offset;
    }

    void append_nodes_and_events(DagGraph & graph) {
        merged_.nodes_.insert(merged_.nodes_.end(), std::make_move_iterator(graph.nodes_.begin()), std::make_move_iterator(graph.nodes_.end()));
        merged_.scope_node_owned_.resize(merged_.nodes_.size(), 0);
        merged_.scope_gap_durations_.resize(merged_.nodes_.size(), 0);
        append_indexed_events(merged_.events_, graph.events_);
    }

    [[nodiscard]] static size_t max_occurrence_count(const HcclRanks & ranks) {
        size_t max_count = 0;
        for (const auto & [graph_index, nodes] : ranks) {
            (void)graph_index;
            max_count = std::max(max_count, nodes.size());
        }
        return max_count;
    }

    [[nodiscard]] std::pair<size_t, size_t> occurrence_shape(const HcclRanks & ranks, size_t occurrence) const {
        size_t participants = 0;
        size_t successors = 0;
        for (const auto & [graph_index, nodes] : ranks) {
            (void)graph_index;
            if (occurrence >= nodes.size()) continue;
            participants++;
            if (merged_.nodes_[nodes[occurrence]].hccl_successor_node_id != DagNode::kNoNode) successors++;
        }
        return { participants, successors };
    }

    [[nodiscard]] size_t count_hccl_edges() const {
        size_t total = 0;
        for (const auto & [name, ranks] : hccl_groups_) {
            (void)name;
            for (size_t occurrence = 0; occurrence < max_occurrence_count(ranks); ++occurrence) {
                const auto [participants, successors] = occurrence_shape(ranks, occurrence);
                if (participants <= 1 || successors == 0) continue;
                if (successors > std::numeric_limits<size_t>::max() / (participants - 1)) {
                    throw std::length_error("HCCL edge count overflow during DAG merge");
                }
                const auto additions = successors * (participants - 1);
                if (additions > std::numeric_limits<size_t>::max() - total) throw std::length_error("HCCL edge count overflow during DAG merge");
                total += additions;
            }
        }
        return total;
    }

    void reserve_and_merge_edges() {
        const auto hccl_edge_count = count_hccl_edges();
        if (hccl_edge_count > std::numeric_limits<size_t>::max() - total_edges_) throw std::length_error("DAG merge edge count overflow");
        merged_.reserve(DagGraphCapacity{ .nodes = total_nodes_, .edges = total_edges_ + hccl_edge_count });
        for (size_t graph_index = 0; graph_index < graphs_.size(); ++graph_index) append_graph_edges(graphs_[graph_index], node_offsets_[graph_index]);
    }

    void append_graph_edges(DagGraph & graph, size_t node_offset) {
        for (auto & edge : graph.edges_) {
            edge.src += node_offset;
            edge.dst += node_offset;
        }
        merged_.edges_.insert(merged_.edges_.end(), std::make_move_iterator(graph.edges_.begin()), std::make_move_iterator(graph.edges_.end()));
    }

    [[nodiscard]] static AlignedHcclNodes aligned_nodes(const HcclRanks & ranks, size_t occurrence) {
        AlignedHcclNodes nodes;
        nodes.reserve(ranks.size());
        for (const auto & [graph_index, rank_nodes] : ranks) {
            if (occurrence < rank_nodes.size()) nodes.emplace_back(graph_index, rank_nodes[occurrence]);
        }
        return nodes;
    }

    [[nodiscard]] uint64_t minimum_nonzero_duration(const AlignedHcclNodes & nodes) const {
        uint64_t minimum = 0;
        for (const auto & [graph_index, node_id] : nodes) {
            (void)graph_index;
            const auto duration = merged_.nodes_[node_id].duration;
            if (duration > 0 && (minimum == 0 || duration < minimum)) minimum = duration;
        }
        return minimum;
    }

    void connect_aligned_nodes(const AlignedHcclNodes & nodes) {
        for (const auto & [graph_index, node_id] : nodes) {
            const auto successor = merged_.nodes_[node_id].hccl_successor_node_id;
            if (successor == DagNode::kNoNode || successor >= merged_.nodes_.size()) continue;
            for (const auto & [peer_graph_index, peer_node_id] : nodes) {
                if (peer_graph_index == graph_index) continue;
                merged_.add_edge(peer_node_id, successor, DagEdgeKind::HCCL);
            }
        }
    }

    void align_hccl_groups() {
        /**
         * Kernel name and occurrence index are the strongest identities present in
         * every supported trace. Peer nodes constrain each rank-local continuation;
         * the minimum nonzero duration is a wait-reduction heuristic, not
         * an independently measured transport service. Do not use this
         * normalized duration as a cost-database training observation.
         * Repeated same-name collectives with divergent rank order remain ambiguous.
         */
        for (const auto & [name, ranks] : hccl_groups_) {
            (void)name;
            for (size_t occurrence = 0; occurrence < max_occurrence_count(ranks); ++occurrence) {
                const auto nodes = aligned_nodes(ranks, occurrence);
                connect_aligned_nodes(nodes);
                const auto minimum = minimum_nonzero_duration(nodes);
                if (minimum > 0) {
                    for (const auto & [graph_index, node_id] : nodes) {
                        (void)graph_index;
                        merged_.set_node_duration(node_id, minimum);
                    }
                }
            }
        }
    }

    std::vector<DagGraph> graphs_;
    DagGraph merged_;
    std::vector<size_t> node_offsets_;
    HcclGroups hccl_groups_;
    size_t total_nodes_ = 0;
    size_t total_edges_ = 0;
#ifdef DEBUG
    uint64_t real_min_ = 0;
    uint64_t real_max_ = 0;
    bool has_real_time_ = false;
#endif
};

DagGraph DagGraph::merge(std::vector<DagGraph> graphs) {
    if (graphs.empty()) return DagGraph();
    if (graphs.size() == 1) return std::move(graphs.front());
    for (const auto & graph : graphs)
        if (!graph.cpu_service_cost().empty() || !graph.retained_cpu_gap_ranges_.empty())
            throw std::invalid_argument("Attach CPU service measurements and retained ranges after merging source graphs");
    return GraphMerger(std::move(graphs)).run();
}

} // namespace markov::trace_graph::core
