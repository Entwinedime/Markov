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

namespace dag_graph_detail {

/**
 * @brief Returns the stable user-visible name for an edge kind.
 *
 * Names are lowercase schema values consumed by run summaries and diagnostics.
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

bool contains_hccl_name(const std::string & name) { return name.contains("hcom") || name.contains("HCCL") || name.contains("hccl"); }

} // namespace dag_graph_detail

using dag_graph_detail::contains_hccl_name;
using dag_graph_detail::edge_kind_name;

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
    if (event_index >= events_.size()) { throw std::out_of_range("event index out of range while adding DAG node"); }
    size_t node_id = nodes_.size();
    const auto & event = events_[event_index];
    DagNode node;
    node.id = node_id;
    node.event_index = event_index;
    node.gpu_id = gpu_id_;
    node.is_cpu = is_cpu;
    node.lane_id = intern_lane(lane_key_value);
    node.duration = event.dur;
    node.original_duration = event.dur;
    nodes_.push_back(node);
    return node_id;
}

size_t DagGraph::add_synthetic_node(const DagSyntheticNodeSpec & spec) {
    if (spec.name.empty()) throw std::invalid_argument("synthetic DAG node name must not be empty");
    TraceEvent event;
    event.index = events_.size();
    event.source_channel = TraceSourceChannel::Synthetic;
    event.event_id = "synthetic_" + std::to_string(nodes_.size());
    event.name = spec.name;
    event.cat = spec.category;
    event.ph = 'X';
    event.dur = spec.duration;
    event.pid = std::to_string(gpu_id_);
    event.tid = spec.lane_key;
    event.set_arg("node_kind", "synthetic");
    for (const auto & [key, value] : spec.attrs) event.set_arg(key, value);
    events_.push_back(std::move(event));

    const auto node_id = add_node(events_.size() - 1, spec.is_cpu, spec.lane_key);
    auto & node = nodes_[node_id];
    node.kind = DagNodeKind::Synthetic;
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
    const auto edge_index = edges_.size();
    edges_.emplace_back(src, dst, kind, effect_id, reason);
    return edge_index;
}

void DagGraph::disable_edge(size_t edge_index) { mutable_edge(edge_index).active = false; }

const DagEdge & DagGraph::edge(size_t edge_index) const {
    if (edge_index >= edges_.size()) throw std::out_of_range("DAG edge index out of range");
    return edges_[edge_index];
}

DagEdge & DagGraph::mutable_edge(size_t edge_index) {
    if (edge_index >= edges_.size()) throw std::out_of_range("DAG edge index out of range");
    return edges_[edge_index];
}

const TraceEvent & DagGraph::event(size_t event_index) const {
    if (event_index >= events_.size()) { throw std::out_of_range("event index out of range"); }
    return events_[event_index];
}

TraceEvent & DagGraph::mutable_event(size_t event_index) {
    if (event_index >= events_.size()) { throw std::out_of_range("event index out of range"); }
    return events_[event_index];
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

size_t DagGraph::active_node_count() const {
    return static_cast<size_t>(std::ranges::count_if(nodes_, [](const auto & node) { return node.active; }));
}

size_t DagGraph::active_trace_node_count() const {
    return static_cast<size_t>(std::ranges::count_if(nodes_, [](const auto & node) { return node.active && node.kind == DagNodeKind::TraceEvent; }));
}

size_t DagGraph::active_synthetic_node_count() const {
    return static_cast<size_t>(std::ranges::count_if(nodes_, [](const auto & node) { return node.active && node.kind == DagNodeKind::Synthetic; }));
}

size_t DagGraph::active_edge_count() const {
    return static_cast<size_t>(std::ranges::count_if(edges_, [&](const auto & edge) {
        return edge.active && edge.src < nodes_.size() && edge.dst < nodes_.size() && nodes_[edge.src].active && nodes_[edge.dst].active;
    }));
}

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

std::unordered_map<std::string, size_t> DagGraph::edge_counts_by_kind() const {
    std::unordered_map<std::string, size_t> counts;
    std::ranges::for_each(edges_, [&](const auto & edge) {
        if (!edge.active || edge.src >= nodes_.size() || edge.dst >= nodes_.size() || !nodes_[edge.src].active || !nodes_[edge.dst].active) return;
        counts[edge_kind_name(edge.kind)]++;
    });
    return counts;
}

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

class DagGraph::GraphMerger {
public:
    explicit GraphMerger(std::vector<DagGraph> graphs) : graphs_(std::move(graphs)), merged_({}, 0), node_offsets_(graphs_.size() + 1, 0) {}

    [[nodiscard]] DagGraph run() {
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
        total_events_ = checked_total([](const auto & graph) { return graph.events_.size(); }, "event");
        total_nodes_ = checked_total([](const auto & graph) { return graph.nodes_.size(); }, "node");
        total_edges_ = checked_total([](const auto & graph) { return graph.edges_.size(); }, "edge");
        merged_.events_.reserve(total_events_);
        merged_.hicache_fact_events_.reserve(checked_total([](const auto & graph) { return graph.hicache_fact_events_.size(); }, "HiCache fact"));
        merged_.context_events_.reserve(checked_total([](const auto & graph) { return graph.context_events_.size(); }, "context event"));
        merged_.tail_context_events_.reserve(
            checked_total([](const auto & graph) { return graph.tail_context_events_.size(); }, "tail context event"));
        merged_.reserve(DagGraphCapacity{ .nodes = total_nodes_, .edges = 0 });
    }

    void merge_nodes_and_events() {
        size_t event_offset = 0;
        size_t node_offset = 0;
        for (size_t graph_index = 0; graph_index < graphs_.size(); ++graph_index) {
            auto & graph = graphs_[graph_index];
            node_offsets_[graph_index] = node_offset;
            merge_parsed_record_count(graph);
            merge_input_contracts(graph);
            for (auto & fact : graph.hicache_fact_events_) {
                fact.index = merged_.hicache_fact_events_.size();
                merged_.hicache_fact_events_.push_back(std::move(fact));
            }
            merged_.context_events_.insert(merged_.context_events_.end(),
                                           std::make_move_iterator(graph.context_events_.begin()),
                                           std::make_move_iterator(graph.context_events_.end()));
            merged_.tail_context_events_.insert(merged_.tail_context_events_.end(),
                                                std::make_move_iterator(graph.tail_context_events_.begin()),
                                                std::make_move_iterator(graph.tail_context_events_.end()));
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
        node_offsets_[graphs_.size()] = node_offset;
#ifdef DEBUG
        merged_.real_e2e_time_ = has_real_time_ && real_max_ > real_min_ ? real_max_ - real_min_ : 0;
#endif
    }

    void merge_input_contracts(const DagGraph & graph) {
        merged_.input_contracts_.insert(merged_.input_contracts_.end(), graph.input_contracts_.begin(), graph.input_contracts_.end());
        std::ranges::sort(merged_.input_contracts_);
        merged_.input_contracts_.erase(std::unique(merged_.input_contracts_.begin(), merged_.input_contracts_.end()), merged_.input_contracts_.end());
    }

    void merge_parsed_record_count(const DagGraph & graph) {
        if (graph.parsed_record_count_ > std::numeric_limits<size_t>::max() - merged_.parsed_record_count_) {
            throw std::length_error("DAG merge parsed-record count overflow");
        }
        merged_.parsed_record_count_ += graph.parsed_record_count_;
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
        if (!node.is_cpu && contains_hccl_name(event.name)) hccl_groups_[event.name][relocation.graph_index].push_back(local_node_id + relocation.node_offset);
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
        for (auto & event : graph.events_) {
            event.index = merged_.events_.size();
            merged_.events_.push_back(std::move(event));
        }
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
            if (minimum == 0 || duration < minimum) minimum = duration;
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
         * the minimum nonzero duration avoids duplicating collective wait time.
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
    size_t total_events_ = 0;
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
    return GraphMerger(std::move(graphs)).run();
}

} // namespace markov::trace_graph::core
