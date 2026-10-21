#include "markov/execution/structure_comparison.hpp"

#include "markov/graph/directed_graph.hpp"
#include "markov/graph/kernel_shapes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <compare>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace markov {
namespace {

using Json = nlohmann::json;
using WorkRank = std::pair<WorkId, std::uint32_t>;
using Endpoint = std::tuple<WorkId, std::uint32_t, std::size_t>;

Json findings() {
    return {
        {     "total",              0 },
        {     "shown",              0 },
        { "by_reason", Json::object() },
        {   "by_kind", Json::object() },
        {  "examples",  Json::array() }
    };
}

void finding(Json & report, const Json & detail) {
    report["total"] = report.at("total").get<std::size_t>() + 1;
    if (detail.is_object()) {
        const auto count = [&](std::string_view category, const Json & key) {
            auto & value = report[category][key.get<std::string>()];
            value = value.is_null() ? 1 : value.get<std::size_t>() + 1;
        };
        if (detail.contains("reason")) count("by_reason", detail.at("reason"));
        if (detail.contains("kind")) count("by_kind", detail.at("kind"));
        else if (detail.contains("work") && detail.at("work").is_object() && detail.at("work").contains("kind")) count("by_kind", detail.at("work").at("kind"));
    }
    if (report.at("examples").size() < 5) {
        report["examples"].push_back(detail);
        report["shown"] = report.at("examples").size();
    }
}

void unobserved_kind(Json & work, std::string_view side, std::string_view kind, const Json & detail) {
    auto & count = work["unobserved_by_kind"][side][kind];
    count = count.is_null() ? 1 : count.get<std::size_t>() + 1;
    auto classified = detail;
    classified["kind"] = kind;
    finding(work["unobserved"], classified);
}

struct ActionKey {
    std::uint32_t rank;
    StepKind step;
    BatchPhase phase;
    std::vector<std::tuple<std::string, std::uint32_t, std::uint32_t, std::uint32_t>> requests;

    auto operator<=>(const ActionKey &) const = default;
};

struct Link {
    std::size_t source;
    std::size_t target;
};

struct ObservedWork {
    SourceSpanId span;
    Json description;
    Json scope;
    bool used = false;
    bool ambiguous = false;
};

struct CollectiveMatch {
    ObservedCollective observation;
    bool operation_used = false;
};

struct CpuInventory {
    std::vector<std::size_t> target;
    std::vector<const ObservedCpuService *> native; // Borrowed from the observation's stable per-action inventory.
};

std::optional<Json> cpu_scope_key(const Json & scope) {
    if (scope.is_null() || scope.at("path").is_null() || (scope.at("binding") != "source_scope" && scope.at("binding") != "target_scope")) return std::nullopt;
    return Json::array({ scope.at("path"), scope.at("model_layer"), scope.at("request_slot"), scope.at("input_slot") });
}

// A pure AND completion names max(contributor ends), not an additional
// consumer. Both graph projections use the same terminal representation.
class CompletionFrontiers {
public:
    CompletionFrontiers(const std::vector<Link> & edges, const std::vector<std::size_t> & order, std::map<BoundaryId, std::vector<BoundaryId>> joins)
        : edges_(edges),
          order_(order),
          joins_(std::move(joins)),
          predecessors_(reverse(edges, order.size())),
          visits_(order.size()) {}

    std::vector<BoundaryId> normalize(const std::vector<BoundaryId> & points) {
        std::vector<BoundaryId> result;
        for (const auto point : points) {
            const auto join = joins_.find(point);
            if (join == joins_.end()) {
                result.push_back(point);
                continue;
            }
            auto found = joined_.find(point);
            if (found == joined_.end()) found = joined_.emplace(point, normalize(join->second)).first;
            // An empty gate cannot disappear and qualify the remaining work.
            if (found->second.empty() || points.size() == 1) return found->second;
            result.insert(result.end(), found->second.begin(), found->second.end());
        }
        return terminal(std::move(result));
    }

private:
    static DirectedGraph reverse(const std::vector<Link> & edges, std::size_t vertices) {
        std::vector<Link> reversed;
        reversed.reserve(edges.size());
        for (const auto & edge : edges) reversed.push_back({ edge.target, edge.source });
        return DirectedGraph(vertices, reversed);
    }

    std::vector<BoundaryId> terminal(std::vector<BoundaryId> points) {
        std::ranges::sort(points);
        points.erase(std::unique(points.begin(), points.end()), points.end());
        if (points.size() < 2) return points;

        if (++generation_ == 0) {
            std::ranges::fill(visits_, 0);
            ++generation_;
        }
        auto earliest = order_[points.front()];
        for (const auto point : points) earliest = std::min(earliest, order_[point]);
        std::vector<BoundaryId> pending;
        const auto predecessors = [&](BoundaryId point) {
            for (const auto index : predecessors_.outgoing(point)) {
                const auto predecessor = edges_[index].source;
                if (order_[predecessor] < earliest || visits_[predecessor] == generation_) continue;
                visits_[predecessor] = generation_;
                pending.push_back(predecessor);
            }
        };
        // One union traversal removes strict ancestors without a pairwise
        // closure. Incomparable contributors remain mandatory.
        for (const auto point : points) predecessors(point);
        for (std::size_t next = 0; next < pending.size(); ++next) predecessors(pending[next]);
        std::erase_if(points, [&](auto point) { return visits_[point] == generation_; });
        return points;
    }

    const std::vector<Link> & edges_;
    const std::vector<std::size_t> & order_;
    std::map<BoundaryId, std::vector<BoundaryId>> joins_;
    DirectedGraph predecessors_;
    std::vector<std::uint32_t> visits_;
    std::uint32_t generation_ = 0;
    std::map<BoundaryId, std::vector<BoundaryId>> joined_;
};

template <class Edge>
bool reaches(const DirectedGraph & graph, const std::vector<Edge> & edges, const std::vector<std::size_t> & order, std::size_t source, std::size_t target) {
    if (source == target) return true;
    if (order[source] > order[target]) return false;
    std::vector<std::size_t> pending{ source };
    std::vector<bool> visited(graph.order().size());
    visited[source] = true;
    for (std::size_t next = 0; next < pending.size(); ++next) {
        if (pending[next] == target) return true;
        for (const auto index : graph.outgoing(pending[next])) {
            const auto point = edges[index].target;
            if (visited[point] || order[point] > order[target]) continue;
            visited[point] = true;
            pending.push_back(point);
        }
    }
    return false;
}

// One side of the comparison, retaining proof only for the current logical
// source. Complete DAG coverage and the named candidate cut are separate: a
// partial member of another AND gate cannot hide downstream dependencies.
class FrontierProjection {
public:
    FrontierProjection(const DirectedGraph & graph, const std::vector<Link> & edges, const std::vector<std::size_t> & order,
                       const std::map<std::size_t, std::vector<std::size_t>> & anchors, const std::map<std::size_t, std::vector<std::size_t>> & frontiers)
        : graph_(graph),
          edges_(edges),
          order_(order),
          anchors_(anchors),
          frontiers_(frontiers),
          visited_(graph.order().size()) {
        for (const auto & [point, names] : anchors_) last_anchor_ = std::max(last_anchor_, order_[point]);
        std::size_t maximum_sources = 1;
        for (const auto & [from, frontier] : frontiers_) maximum_sources = std::max(maximum_sources, frontier.size());
        coverage_stride_ = (maximum_sources + 63) / 64;
        coverage_.resize(graph_.order().size() * coverage_stride_);
    }

    void from(std::size_t source) {
        for (const auto point : proof_points_)
            for (std::size_t word = 0; word < coverage_words_; ++word) coverage_[point * coverage_stride_ + word] = 0;
        proof_points_.clear();
        proved_ = false;
        from_ = source;
        sources_ = &frontiers_.at(source);
        coverage_words_ = (sources_->size() + 63) / 64;
    }

    bool precedes(std::size_t to) {
        const auto & targets = frontiers_.at(to);
        if (sources_->empty() || targets.empty()) return false;
        prove();
        // time = max(frontier): each source may reach a different destination
        // member. Union target memberships, then require every source bit.
        for (std::size_t word = 0; word < coverage_words_; ++word) {
            std::uint64_t members = 0;
            for (const auto point : targets) members |= coverage_[point * coverage_stride_ + word];
            const auto remaining = sources_->size() - word * 64;
            const auto required = remaining >= 64 ? ~std::uint64_t{} : (std::uint64_t{ 1 } << remaining) - 1;
            if (members != required) return false;
        }
        return true;
    }

    std::set<std::size_t> successors() {
        std::set<std::size_t> paths;
        if (sources_->empty()) return paths;
        const auto singleton = sources_->size() == 1;
        if (!singleton) prove();
        next_walk();
        std::vector<std::size_t> pending(sources_->begin(), sources_->end());
        for (const auto point : *sources_) visited_[point] = generation_;
        std::map<std::size_t, bool> encountered;
        for (std::size_t next = 0; next < pending.size(); ++next)
            for (const auto index : graph_.outgoing(pending[next])) {
                const auto point = edges_[index].target;
                if (visited_[point] == generation_ || order_[point] > last_anchor_) continue;
                visited_[point] = generation_;
                bool stop = false;
                if (const auto found = anchors_.find(point); found != anchors_.end()) {
                    for (const auto to : found->second) {
                        if (from_ == to) continue;
                        // The BFS path itself proves a singleton origin is
                        // before this member of the destination AND gate.
                        if (!singleton) {
                            auto known = encountered.find(to);
                            if (known == encountered.end()) known = encountered.emplace(to, precedes(to)).first;
                            if (!known->second) continue;
                        }
                        paths.insert(to);
                        if (frontiers_.at(to).size() == 1) stop = true;
                    }
                }
                if (!stop) pending.push_back(point);
            }
        return paths;
    }

private:
    void next_walk() {
        if (++generation_ == 0) {
            std::ranges::fill(visited_, 0);
            ++generation_;
        }
    }

    void prove() {
        if (proved_) return;
        next_walk();
        const auto singleton = sources_->size() == 1;
        proof_points_.assign(sources_->begin(), sources_->end());
        for (std::size_t source = 0; source < sources_->size(); ++source) {
            const auto point = (*sources_)[source];
            visited_[point] = generation_;
            coverage_[point * coverage_stride_ + source / 64] |= std::uint64_t{ 1 } << (source % 64);
        }
        // Proof crosses every named cut and is reused by all checks from this
        // origin on both sides. Candidate stopping never truncates the proof.
        for (std::size_t next = 0; next < proof_points_.size(); ++next)
            for (const auto index : graph_.outgoing(proof_points_[next])) {
                const auto point = edges_[index].target;
                if (visited_[point] == generation_ || order_[point] > last_anchor_) continue;
                visited_[point] = generation_;
                proof_points_.push_back(point);
                if (singleton) coverage_[point * coverage_stride_] = 1;
            }
        if (!singleton) {
            std::ranges::sort(proof_points_, {}, [&](auto point) { return order_[point]; });
            for (const auto point : proof_points_)
                for (const auto index : graph_.outgoing(point)) {
                    const auto target = edges_[index].target;
                    if (visited_[target] != generation_) continue;
                    for (std::size_t word = 0; word < coverage_words_; ++word)
                        coverage_[target * coverage_stride_ + word] |= coverage_[point * coverage_stride_ + word];
                }
        }
        proved_ = true;
    }

    const DirectedGraph & graph_;
    const std::vector<Link> & edges_;
    const std::vector<std::size_t> & order_;
    const std::map<std::size_t, std::vector<std::size_t>> & anchors_;
    const std::map<std::size_t, std::vector<std::size_t>> & frontiers_;
    std::size_t last_anchor_ = 0;
    std::size_t coverage_stride_ = 1;
    std::size_t coverage_words_ = 0;
    std::size_t from_ = 0;
    const std::vector<std::size_t> * sources_ = nullptr;
    std::vector<std::uint32_t> visited_;
    std::uint32_t generation_ = 0;
    std::vector<std::uint64_t> coverage_;
    std::vector<std::size_t> proof_points_;
    bool proved_ = false;
};

Json kernel_work(const SourceKernel & kernel) {
    return {
        {           "kind",                                    "kernel" },
        {           "name",                                 kernel.name },
        {      "operation",                            kernel.operation },
        {   "input_shapes",  KernelShapes(kernel.input_shapes).encode() },
        {  "output_shapes", KernelShapes(kernel.output_shapes).encode() },
        {   "input_dtypes",                         kernel.input_dtypes },
        {  "output_dtypes",                        kernel.output_dtypes },
        {  "input_formats",                        kernel.input_formats },
        { "output_formats",                       kernel.output_formats }
    };
}

Json target_work(const Json & node) {
    if (!node.at("kernel").is_null()) {
        const auto & kernel = node.at("kernel");
        return kernel_work({ kernel.at("name"),
                             kernel.at("operation"),
                             kernel.at("input_shapes"),
                             kernel.at("output_shapes"),
                             kernel.at("input_dtypes"),
                             kernel.at("output_dtypes"),
                             kernel.at("input_formats"),
                             kernel.at("output_formats") });
    }
    if (node.contains("transfer"))
        return {
            {      "kind",                          "transfer" },
            {     "bytes",     node.at("transfer").at("bytes") },
            { "direction", node.at("transfer").at("direction") }
        };
    if (node.contains("device_command"))
        return {
            {      "kind",                          "device_command" },
            { "task_type", node.at("device_command").at("task_type") }
        };
    return nullptr;
}

bool compatible_scope(const Json & target, const Json & observed) {
    if (target.is_null() || target.at("path").is_null() || observed.at("path").is_null()) return true;
    const auto & prefix = target.at("path");
    const auto & path = observed.at("path");
    if (prefix.size() > path.size() || !std::equal(prefix.begin(), prefix.end(), path.begin())) return false;
    if ((target.at("binding") == "source_scope" || target.at("binding") == "target_scope") && prefix.size() != path.size()) return false;
    for (const auto field : { "request_slot", "model_layer", "input_slot" })
        if (!target.at(field).is_null() && !observed.at(field).is_null() && target.at(field) != observed.at(field)) return false;
    return true;
}

} // namespace

nlohmann::json StructureComparison::run(const std::filesystem::path & target_graph) const {
    const ServingContract contract;
    // The cut already projects over omitted CPU annotations. Preserve those
    // known paths instead of trying to reconstruct the dropped observations.
    std::vector<Link> native_edges;
    native_edges.reserve(graph_.edges().size());
    for (const auto & edge : graph_.edges()) native_edges.push_back({ edge.source, edge.target });
    for (const auto & block : subgraphs_.blocks)
        for (const auto & path : block.thread_paths) native_edges.push_back({ path[0], path[1] });
    // A paired native group supplies actual entry/return cuts. Its adapter's
    // protocol rule derives the necessary completion links between those cuts;
    // they are not an independent raw transport observation or launch barrier.
    for (std::size_t call = 0; call < calls_.size(); ++call) {
        if (calls_[call].is_annotation()) continue;
        for (const auto & collective : semantics_.collectives(call)) {
            if (collective.work.host_message) {
                for (const auto entry : collective.entries)
                    for (const auto completion : collective.completions) native_edges.push_back({ entry, completion });
                for (const auto & [entry, completion] : collective.completion_entry_dependencies) native_edges.push_back({ entry, completion });
            }
        }
    }
    const DirectedGraph native_paths(graph_.boundaries().size(), native_edges);
    if (!native_paths.acyclic())
        return {
            {   "status",                      "not_comparable" },
            {   "reason", "observed_dependencies_contain_cycle" },
            { "coverage",                 semantics_.coverage() }
        };
    std::vector<std::size_t> native_order(graph_.boundaries().size());
    for (std::size_t index = 0; index < native_paths.order().size(); ++index) native_order[native_paths.order()[index]] = index;

    // complete() creates max(contributor ends), not a physical consumer of the
    // joined work. Only pure completion gates may be replaced by that frontier.
    std::map<BoundaryId, std::vector<BoundaryId>> native_joins;
    for (BoundaryId point = 0; point < graph_.boundaries().size(); ++point) {
        const auto domain = graph_.lanes()[graph_.boundaries()[point].lane].domain;
        if (domain == SourceDomain::LogicalCompletion || domain == SourceDomain::DeviceTaskCompletion) native_joins.emplace(point, std::vector<BoundaryId>{});
    }
    std::set<BoundaryId> mixed_joins;
    for (const auto & edge : graph_.edges()) {
        const auto join = native_joins.find(edge.target);
        if (join == native_joins.end()) continue;
        if (edge.relation == SourceRelation::CompletionJoin) join->second.push_back(edge.source);
        else mixed_joins.insert(edge.target);
    }
    for (const auto point : mixed_joins) native_joins.erase(point);

    CompletionFrontiers native_completions(native_edges, native_order, std::move(native_joins));

    std::map<std::size_t, const BlockSubgraph *> cuts;
    for (const auto & block : subgraphs_.blocks) cuts.emplace(block.call, &block);
    std::map<ActionKey, std::deque<std::size_t>> actions;
    std::vector<std::size_t> ordered;
    for (std::size_t index = 0; index < calls_.size(); ++index)
        if (!calls_[index].is_annotation()) ordered.push_back(index);
    std::ranges::sort(ordered, {}, [&](auto index) { return calls_[index].observed_start_ns; });
    for (const auto index : ordered) {
        const auto & call = calls_[index];
        ActionKey key{ call.rank, call.step, call.phase.value_or(BatchPhase::Idle), {} };
        for (const auto & request : call.requests)
            key.requests.emplace_back(request.id, request.input_tokens, request.context_tokens, request.generated_tokens);
        actions[std::move(key)].push_back(index);
    }

    Json result{
        {         "status",                                                                                                                        "compared" },
        {        "control",                                                                       { { "matched_actions", 0 }, { "differences", findings() } } },
        {           "work", { { "matched_nodes", 0 }, { "differences", findings() }, { "unobserved", findings() }, { "unobserved_by_kind", Json::object() } } },
        {         "causal",                                           { { "checked_paths", 0 }, { "differences", findings() }, { "unobserved", findings() } } },
        {         "effect",                                                                                                 { { "differences", findings() } } },
        { "state_coverage",                                         { { "checks", findings() }, { "differences", findings() }, { "unobserved", findings() } } },
        {       "coverage",                                                                                                             semantics_.coverage() },
        {           "gaps",                                                                                                                        findings() }
    };
    for (const auto & gap : subgraphs_.gaps) finding(result["gaps"], gap);
    std::map<Endpoint, std::vector<BoundaryId>> endpoints;
    std::map<std::tuple<WorkId, std::uint32_t, std::string>, std::vector<BoundaryId>> published_ports;
    std::map<Json, std::size_t> cpu_lanes;
    std::map<std::size_t, Json> cpu_resources;
    std::map<std::uint32_t, std::int64_t> last_schedule;
    std::set<std::pair<WorkId, std::uint32_t>> incomplete_scopes;
    std::set<SourceSpanId> transport_tasks;
    for (const auto & [span, collective] : graph_.collectives())
        for (const auto & task : collective.tasks) transport_tasks.insert(task.span);
    std::size_t batches = 0;

    std::ifstream stream(target_graph);
    stream.exceptions(std::ios::badbit);
    if (!stream) throw std::runtime_error("cannot open fixed target graph: " + target_graph.string());
    for (std::string line; std::getline(stream, line);) {
        const auto record = Json::parse(line);
        if (record.at("record") != "batch") continue;
        ++batches;
        std::vector<Link> edges;
        std::map<BoundaryId, std::vector<BoundaryId>> target_joins;
        std::set<BoundaryId> mixed_target_joins;
        for (const auto & edge : record.at("edges")) {
            const auto source = edge.at("source").get<BoundaryId>(), target = edge.at("target").get<BoundaryId>();
            edges.push_back({ source, target });
            // CompletionJoin is emitted only for synthetic max gates. Any
            // additional incoming semantics makes the point non-expandable.
            if (edge.at("relation") == SourceGraph::relation_name(SourceRelation::CompletionJoin)) target_joins[target].push_back(source);
            else mixed_target_joins.insert(target);
        }
        for (const auto point : mixed_target_joins) target_joins.erase(point);
        const DirectedGraph target_paths(record.at("boundaries").size(), edges);
        if (!target_paths.acyclic())
            return {
                { "status",                          "not_comparable" },
                { "reason", "fixed_target_dependencies_contain_cycle" }
            };
        std::map<WorkRank, std::size_t> matches;
        std::map<WorkRank, std::vector<CollectiveMatch>> collectives;
        std::set<WorkRank> mismatched_states;
        std::map<WorkRank, Json> resource_nodes;
        for (const auto & node : record.at("nodes")) {
            if (node.at("kind") != SourceGraph::span_name(SourceSpanKind::ResourceObservation)) continue;
            const WorkRank work{ node.at("work_id"), node.at("rank") };
            resource_nodes.try_emplace(work, Json::array()).first->second.push_back(node);
        }
        const auto no_resources = Json::array();
        std::map<std::size_t, std::vector<BoundaryId>> boundary_matches;
        std::set<std::size_t> ambiguous_boundaries;
        const auto bind_frontier = [&](std::size_t target, std::vector<BoundaryId> native) {
            if (ambiguous_boundaries.contains(target)) return;
            native = native_completions.normalize(native);
            const auto [found, added] = boundary_matches.emplace(target, native);
            if (added && native.empty())
                finding(result["causal"]["unobserved"],
                        {
                            {   "reason", "native_completion_frontier_not_observed" },
                            {    "batch",                        record.at("batch") },
                            { "boundary",                                    target }
                });
            if (!added && found->second != native) {
                boundary_matches.erase(found);
                ambiguous_boundaries.insert(target);
            }
        };
        const auto bind_boundary = [&](std::size_t target, BoundaryId native) { bind_frontier(target, { native }); };

        for (const auto & invocation : record.at("invocations")) {
            const auto work_id = invocation.at("work_id").get<WorkId>();
            const auto step = contract.block(invocation.at("step").get<std::string>()).kind;
            const auto phase = invocation.at("phase").get<std::string>();
            const auto batch_phase = phase == contract.phase_name(BatchPhase::Prefill)  ? BatchPhase::Prefill
                                     : phase == contract.phase_name(BatchPhase::Decode) ? BatchPhase::Decode
                                                                                        : BatchPhase::Idle;
            if (phase != contract.phase_name(batch_phase)) throw std::invalid_argument("unknown target batch phase: " + phase);
            for (const auto & instance : record.at("instances")) {
                if (instance.at("work_id") != work_id) continue;
                const auto rank = instance.at("rank").get<std::uint32_t>();
                ActionKey key{ rank, step, batch_phase, {} };
                for (const auto & request : invocation.at("requests"))
                    key.requests.emplace_back(request.at("id"), request.at("input_tokens"), request.at("context_tokens"), request.at("generated_tokens"));
                const auto found = actions.find(key);
                if (found == actions.end() || found->second.empty()) {
                    finding(result["control"]["differences"],
                            {
                                {  "reason", "target_action_not_observed" },
                                { "work_id",                      work_id },
                                {    "rank",                         rank },
                                {    "step",        invocation.at("step") },
                                {   "phase",                        phase }
                    });
                    continue;
                }
                const auto call = found->second.front();
                found->second.pop_front();
                matches.emplace(WorkRank{ work_id, rank }, call);
                for (auto & collective : semantics_.collectives(call)) collectives[{ work_id, rank }].push_back({ std::move(collective) });
                result["control"]["matched_actions"] = result["control"]["matched_actions"].get<std::size_t>() + 1;
                if (step == StepKind::Schedule) {
                    const auto [previous, first] = last_schedule.emplace(rank, calls_[call].observed_start_ns);
                    if (!first && previous->second >= calls_[call].observed_start_ns)
                        finding(result["control"]["differences"],
                                {
                                    {  "reason", "native_decision_order_differs" },
                                    { "work_id",                         work_id },
                                    {    "rank",                            rank }
                        });
                    previous->second = calls_[call].observed_start_ns;
                }
                const auto resources = resource_nodes.find({ work_id, rank });
                const auto state = semantics_.compare_state(call, invocation, resources == resource_nodes.end() ? no_resources : resources->second);
                for (const auto & resource : state.at("resource_boundaries")) {
                    bind_boundary(resource.at("target_begin"), resource.at("native_begin"));
                    bind_boundary(resource.at("target_end"), resource.at("native_end"));
                }
                for (const auto & field : state.at("checked")) finding(result["state_coverage"]["checks"], field);
                for (const auto category : { "unobserved", "differences" })
                    for (const auto & item : state.at(category))
                        finding(result["state_coverage"][category],
                                {
                                    { "work_id", work_id },
                                    {    "rank",    rank },
                                    {  "detail",    item }
                        });
                for (const auto & effect : state.at("effects"))
                    finding(result["effect"]["differences"],
                            {
                                { "work_id", work_id },
                                {    "rank",    rank },
                                {  "detail",  effect }
                    });
                if (!state.at("differences").empty()) mismatched_states.emplace(work_id, rank);
            }
        }

        std::map<WorkRank, std::vector<ObservedWork>> observed;
        for (const auto & [work, call] : matches) {
            const auto cut = cuts.find(call);
            if (cut == cuts.end() || mismatched_states.contains(work)) continue;
            auto & nodes = observed[work];
            std::set<SourceSpanId> logical_collectives;
            for (const auto & unit : collectives[work]) logical_collectives.insert(unit.observation.source_collective);
            const auto implementations = semantics_.collective_implementations(call);
            std::map<SourceSpanId, const ObservedCollectiveImplementation *> implementation_owners;
            for (const auto & implementation : implementations)
                for (const auto span : implementation.spans) implementation_owners.emplace(span, &implementation);
            for (const auto span : cut->second->spans) {
                const auto kind = graph_.spans()[span].kind;
                // Waits, state markers and communication annotations are not
                // additional services. Keep their causal vertices in the DAG;
                // collective hardware is checked through its actual ownership.
                if (kind == SourceSpanKind::HostWait || kind == SourceSpanKind::DeviceWait || kind == SourceSpanKind::ResourceObservation
                    || kind == SourceSpanKind::CommunicationObservation)
                    continue;
                if (const auto owner = implementation_owners.find(span); owner != implementation_owners.end()) {
                    if (!owner->second->qualified)
                        unobserved_kind(result["work"],
                                        "observed",
                                        SourceGraph::span_name(kind),
                                        {
                                            {          "reason", "collective_implementation_not_qualified" },
                                            {         "work_id",                                work.first },
                                            {            "rank",                               work.second },
                                            { "collective_call",      calls_[owner->second->call].identity },
                                            {     "source_span",                                      span }
                        });
                    continue;
                }
                // Logical collectives are a separate projection. Physical
                // transport tasks must not masquerade as missing model kernels.
                if (transport_tasks.contains(span)) continue;
                if (graph_.collectives().contains(span)) {
                    if (!logical_collectives.contains(span))
                        unobserved_kind(result["work"],
                                        "observed",
                                        "collective",
                                        {
                                            {     "work_id",                             work.first },
                                            {        "rank",                            work.second },
                                            {      "reason", "native_collective_identity_not_bound" },
                                            { "source_span",                                   span }
                        });
                    continue;
                }
                if (const auto kernel = graph_.kernels().find(span); kernel != graph_.kernels().end())
                    nodes.push_back({ span, kernel_work(kernel->second), semantics_.scope(graph_.spans()[span].scope_call.value_or(call)) });
                else if (const auto transfer = graph_.transfers().find(span); transfer != graph_.transfers().end())
                    nodes.push_back({
                        span,
                        { { "kind", "transfer" },
                          { "bytes", transfer->second.bytes },
                          { "direction", SourceTransfer::direction_name(transfer->second.direction) } },
                        semantics_.scope(graph_.spans()[span].scope_call.value_or(call))
                    });
                else if (const auto command = semantics_.command(span))
                    nodes.push_back({
                        span,
                        { { "kind", "device_command" }, { "task_type", command->task_type } },
                        semantics_.scope(graph_.spans()[span].scope_call.value_or(call))
                    });
                else if (graph_.spans()[span].kind != SourceSpanKind::CpuControl && graph_.spans()[span].kind != SourceSpanKind::CpuObservation)
                    unobserved_kind(result["work"],
                                    "observed",
                                    SourceGraph::span_name(graph_.spans()[span].kind),
                                    {
                                        {     "work_id",  work.first },
                                        {        "rank", work.second },
                                        { "source_span",        span }
                    });
            }
            std::ranges::sort(nodes, {}, [&](const auto & node) { return native_order[graph_.spans()[node.span].begin]; });
        }
        std::vector<std::size_t> target_order(record.at("boundaries").size());
        for (std::size_t index = 0; index < target_paths.order().size(); ++index) target_order[target_paths.order()[index]] = index;
        CompletionFrontiers target_completions(edges, target_order, std::move(target_joins));

        using CpuKey = std::pair<WorkRank, Json>;
        std::map<CpuKey, CpuInventory> cpu_inventory;
        std::set<std::pair<WorkRank, Json>> incomplete_cpu_scopes;
        std::map<std::size_t, Json> target_cpu_resources;
        for (const auto & [work, call] : matches) {
            if (mismatched_states.contains(work)) continue;
            for (const auto & service : semantics_.cpu_services(call)) {
                const auto begin = cpu_scope_key(service.begin_scope);
                const auto end = cpu_scope_key(service.end_scope);
                const auto descriptor = service.service.describe();
                if (!begin || !end) {
                    incomplete_cpu_scopes.emplace(work, descriptor);
                    unobserved_kind(result["work"],
                                    "observed",
                                    "cpu_service",
                                    {
                                        {  "reason", "cpu_service_semantic_scope_not_observed" },
                                        { "work_id",                                work.first },
                                        {    "rank",                               work.second },
                                        { "service",                                descriptor },
                                        {   "begin",                             service.begin },
                                        {     "end",                               service.end }
                    });
                    continue;
                }
                cpu_inventory[{ work, Json::array({ descriptor, *begin, *end }) }].native.push_back(&service);
            }
        }
        for (std::size_t index = 0; index < record.at("nodes").size(); ++index) {
            const auto & node = record.at("nodes")[index];
            const WorkRank work{ node.at("work_id"), node.at("rank") };
            if (!node.contains("cpu") || node.at("role") != "service" || !matches.contains(work) || mismatched_states.contains(work)) continue;
            const Json *begin_scope = &node.at("logical_scope"), *end_scope = begin_scope;
            if (!begin_scope->is_null() && begin_scope->at("binding") == "cpu_aggregate") {
                const auto & members = begin_scope->at("member_nodes");
                begin_scope = members.empty() ? nullptr : &record.at("nodes").at(members.front().get<std::size_t>()).at("logical_scope");
                end_scope = members.empty() ? nullptr : &record.at("nodes").at(members.back().get<std::size_t>()).at("logical_scope");
            }
            const auto begin = begin_scope ? cpu_scope_key(*begin_scope) : std::nullopt;
            const auto end = end_scope ? cpu_scope_key(*end_scope) : std::nullopt;
            const auto & descriptor = node.at("cpu").at("service");
            if (!begin || !end) {
                incomplete_cpu_scopes.emplace(work, descriptor);
                unobserved_kind(result["work"],
                                "target",
                                "cpu_service",
                                {
                                    {  "reason", "cpu_service_semantic_scope_not_observed" },
                                    { "work_id",                                work.first },
                                    {    "rank",                               work.second },
                                    { "service",                                descriptor },
                                    {    "node",                                     index }
                });
                continue;
            }
            cpu_inventory[{ work, Json::array({ descriptor, *begin, *end }) }].target.push_back(index);
            const auto resource = node.find("cpu_resource");
            if (resource == node.end() || resource->is_null()) continue;
            auto key = *resource;
            // Common TemplateResource identity ignores provenance IDs for a qualified role.
            if (!key.at("role").is_null()) key["native_id"] = 0;
            target_cpu_resources.emplace(index, std::move(key));
        }
        // A later group may name the worker lanes needed by an earlier one.
        // Retry only deferred groups while aliases grow, then report unknowns.
        std::vector<decltype(cpu_inventory)::const_iterator> pending_cpu;
        for (auto group = cpu_inventory.cbegin(); group != cpu_inventory.cend(); ++group) pending_cpu.push_back(group);
        bool finalize_cpu = false;
        while (!pending_cpu.empty()) {
            const auto aliases_before = cpu_lanes.size();
            std::vector<decltype(cpu_inventory)::const_iterator> deferred_cpu;
            for (const auto group : pending_cpu) {
                const auto & [key, inventory] = *group;
                const auto & [work, description] = key;
                const auto detail = [&](std::string_view reason) {
                    return Json{
                        {         "reason",                  reason },
                        {           "kind",           "cpu_service" },
                        {        "work_id",              work.first },
                        {           "rank",             work.second },
                        {        "service",             description },
                        {   "target_count", inventory.target.size() },
                        { "observed_count", inventory.native.size() }
                    };
                };
                const auto unknown = [&](std::string_view reason) {
                    auto item = detail(reason);
                    for (const auto index : inventory.target) {
                        item["node"] = index;
                        unobserved_kind(result["work"], "target", "cpu_service", item);
                    }
                    item.erase("node");
                    for (const auto * service : inventory.native) {
                        item["begin"] = service->begin;
                        item["end"] = service->end;
                        unobserved_kind(result["work"], "observed", "cpu_service", item);
                    }
                };
                if (finalize_cpu) {
                    unknown("cpu_service_has_no_unique_thread_or_causal_mapping");
                    continue;
                }
                if (inventory.target.size() != inventory.native.size()) {
                    if (incomplete_cpu_scopes.contains({ work, description.at(0) })) unknown("cpu_service_scope_prevents_inventory_comparison");
                    else
                        finding(result["work"]["differences"],
                                detail(inventory.target.size() > inventory.native.size() ? "target_cpu_service_not_observed"
                                                                                         : "observed_cpu_service_not_in_target"));
                    continue;
                }
                if (std::ranges::any_of(inventory.target, [&](std::size_t index) { return !target_cpu_resources.contains(index); })) {
                    unknown("target_cpu_thread_resource_not_observed");
                    continue;
                }

                std::map<std::size_t, CpuInventory> lanes;
                const bool known_lanes =
                    std::ranges::all_of(inventory.target, [&](std::size_t index) { return cpu_lanes.contains(target_cpu_resources.at(index)); });
                for (const auto index : inventory.target) lanes[known_lanes ? cpu_lanes.at(target_cpu_resources.at(index)) : 0].target.push_back(index);
                for (const auto * service : inventory.native) lanes[known_lanes ? service->native_lane : 0].native.push_back(service);
                std::vector<std::pair<std::size_t, const ObservedCpuService *>> pairs;
                bool unique = true;
                for (auto & [lane, members] : lanes) {
                    if (members.target.size() != members.native.size()) {
                        unique = false;
                        break;
                    }
                    std::ranges::sort(members.target, {}, [&](std::size_t index) {
                        return target_order[record.at("nodes")[index].at("begin").get<std::size_t>()];
                    });
                    std::ranges::sort(members.native, {}, [&](const ObservedCpuService * service) { return native_order[service->begin]; });
                    for (std::size_t index = 0; index < members.target.size(); ++index) {
                        if (index != 0) {
                            const auto & previous = record.at("nodes")[members.target[index - 1]];
                            const auto & next = record.at("nodes")[members.target[index]];
                            const auto * before = members.native[index - 1];
                            const auto * after = members.native[index];
                            // Topological sorting alone cannot disambiguate concurrent copies.
                            if (!reaches(target_paths, edges, target_order, previous.at("end"), next.at("begin"))
                                || !reaches(native_paths, native_edges, native_order, before->end, after->begin)) {
                                unique = false;
                                break;
                            }
                        }
                        pairs.emplace_back(members.target[index], members.native[index]);
                    }
                    if (!unique) break;
                }
                if (!unique) {
                    if (!known_lanes) deferred_cpu.push_back(group);
                    else unknown("cpu_service_has_no_unique_thread_or_causal_mapping");
                    continue;
                }
                // A rejected group cannot establish aliases used by later groups.
                auto proposed_lanes = cpu_lanes;
                auto proposed_resources = cpu_resources;
                bool aliases = true;
                for (const auto & [index, service] : pairs) {
                    const auto & resource = target_cpu_resources.at(index);
                    const auto [lane, new_resource] = proposed_lanes.emplace(resource, service->native_lane);
                    const auto [reverse, new_lane] = proposed_resources.emplace(service->native_lane, resource);
                    if (lane->second != service->native_lane || reverse->second != resource) {
                        aliases = false;
                        break;
                    }
                }
                if (!aliases) {
                    finding(result["work"]["differences"], detail("cpu_thread_alias_differs"));
                    continue;
                }
                cpu_lanes = std::move(proposed_lanes);
                cpu_resources = std::move(proposed_resources);
                for (const auto & [index, service] : pairs) {
                    const auto & node = record.at("nodes")[index];
                    bind_boundary(node.at("begin"), service->begin);
                    bind_boundary(node.at("end"), service->end);
                    result["work"]["matched_nodes"] = result["work"]["matched_nodes"].get<std::size_t>() + 1;
                }
            }
            pending_cpu = std::move(deferred_cpu);
            finalize_cpu = cpu_lanes.size() == aliases_before;
        }

        std::vector<std::size_t> target_nodes(record.at("nodes").size());
        std::map<WorkRank, std::map<Json, std::size_t>> target_inventory;
        for (std::size_t index = 0; index < target_nodes.size(); ++index) target_nodes[index] = index;
        std::ranges::stable_sort(target_nodes, {}, [&](std::size_t index) { return target_order[record.at("nodes")[index].at("begin").get<std::size_t>()]; });
        for (const auto index : target_nodes) {
            const auto & node = record.at("nodes")[index];
            const WorkRank work{ node.at("work_id"), node.at("rank") };
            if (!matches.contains(work) || mismatched_states.contains(work)) continue;
            if (node.at("kind") == SourceGraph::span_name(SourceSpanKind::HostWait) || node.at("kind") == SourceGraph::span_name(SourceSpanKind::DeviceWait))
                continue;
            if (node.at("role") == "annotation") {
                if (node.at("kind") != SourceGraph::span_name(SourceSpanKind::CpuControl)
                    && node.at("kind") != SourceGraph::span_name(SourceSpanKind::CpuObservation)
                    && node.at("kind") != SourceGraph::span_name(SourceSpanKind::ResourceObservation)
                    && node.at("kind") != SourceGraph::span_name(SourceSpanKind::CommunicationObservation))
                    unobserved_kind(result["work"],
                                    "target",
                                    node.at("kind").get<std::string>(),
                                    {
                                        { "work_id",  work.first },
                                        {    "rank", work.second }
                    });
                continue;
            }
            if (node.contains("collective")) {
                const auto & expected = node.at("collective");
                const auto scope = node.value("logical_scope", Json(nullptr));
                const auto rank = expected.at("rank").get<std::uint32_t>();
                auto & units = collectives[{ work.first, rank }];
                const auto found = std::ranges::find_if(units, [&](const auto & item) {
                    const auto & actual_scope = item.observation.scope;
                    if (item.operation_used || scope.is_null() || !scope.contains("collective_position")
                        || actual_scope.at("collective_position") != scope.at("collective_position") || !compatible_scope(scope, actual_scope))
                        return false;
                    auto actual = item.observation.work.describe();
                    actual["rank"] = rank;
                    actual["execution"] = expected.at("execution");
                    return actual == expected;
                });
                if (found != units.end()) {
                    found->operation_used = true;
                    const auto & observed = found->observation;
                    if (!observed.entries.empty() && !observed.completions.empty()) {
                        bind_frontier(node.at("begin"), observed.entries);
                        bind_frontier(node.at("end"), observed.completions);
                        result["work"]["matched_nodes"] = result["work"]["matched_nodes"].get<std::size_t>() + 1;
                        continue;
                    }
                }
                else
                    unobserved_kind(result["work"],
                                    "target",
                                    "collective",
                                    {
                                        { "work_id",                                   work.first },
                                        {    "rank",                                         rank },
                                        {  "reason", "collective_interface_not_uniquely_observed" }
                    });
                finding(result["causal"]["unobserved"],
                        {
                            { "work_id",                                             work.first },
                            {    "rank",                                            work.second },
                            {  "reason", "collective_submission_or_completion_frontier_missing" }
                });
                continue;
            }
            const auto description = target_work(node);
            if (description.is_null()) {
                // Named CPU services were checked above. Other work stays explicit.
                if (!node.contains("cpu"))
                    unobserved_kind(result["work"],
                                    "target",
                                    node.at("kind").get<std::string>(),
                                    {
                                        { "work_id",  work.first },
                                        {    "rank", work.second }
                    });
                continue;
            }
            ++target_inventory[work][description];
            const auto scope = node.value("logical_scope", Json(nullptr));
            if ((scope.is_null() || scope.at("path").is_null() || scope.at("binding") == "recipient_owner") && incomplete_scopes.insert(work).second)
                finding(result["work"]["unobserved"],
                        {
                            { "work_id",                                 work.first },
                            {    "rank",                                work.second },
                            {  "reason", "target_inner_semantic_identity_not_bound" }
                });
            auto & candidates = observed[work];
            const auto found = std::ranges::find_if(candidates, [&](const auto & item) {
                return !item.used && item.description == description && compatible_scope(scope, item.scope);
            });
            if (found == candidates.end()) {
                finding(result["work"]["differences"],
                        {
                            {  "reason", "target_work_not_observed" },
                            { "work_id",                 work.first },
                            {    "rank",                work.second },
                            {    "work",                description }
                });
                continue;
            }
            bool ambiguous = found->ambiguous;
            for (auto next = std::next(found); next != candidates.end(); ++next) {
                if (next->used || next->description != description || !compatible_scope(scope, next->scope)) continue;
                if (!reaches(native_paths, native_edges, native_order, graph_.spans()[found->span].end, graph_.spans()[next->span].begin)) {
                    next->ambiguous = true;
                    ambiguous = true;
                }
            }
            if (ambiguous) {
                found->ambiguous = true;
                finding(result["work"]["unobserved"],
                        {
                            { "work_id",                                   work.first },
                            {    "rank",                                  work.second },
                            {    "node",                                        index },
                            {  "reason", "repeated_work_has_no_unique_causal_mapping" }
                });
                continue;
            }
            bool incomplete_scope = found->scope.at("path").is_null();
            if (!scope.is_null())
                for (const auto field : { "request_slot", "model_layer" })
                    incomplete_scope = incomplete_scope || (scope.at(field).is_null() != found->scope.at(field).is_null());
            if (incomplete_scope && incomplete_scopes.insert(work).second)
                finding(result["work"]["unobserved"],
                        {
                            { "work_id",                             work.first },
                            {    "rank",                            work.second },
                            {  "reason", "semantic_position_not_fully_observed" }
                });
            found->used = true;
            const auto & span = graph_.spans()[found->span];
            bind_boundary(node.at("begin"), span.begin);
            bind_boundary(node.at("end"), span.end);
            result["work"]["matched_nodes"] = result["work"]["matched_nodes"].get<std::size_t>() + 1;
        }
        for (const auto & [work, nodes] : observed) {
            // An ambiguous mapping does not make the amount of known work
            // ambiguous. Count complete work descriptors before checking seams.
            std::map<Json, std::pair<std::size_t, bool>> inventory;
            for (const auto & node : nodes) {
                auto & [count, ambiguous] = inventory[node.description];
                ++count;
                ambiguous = ambiguous || node.ambiguous;
            }
            for (const auto & [description, count] : inventory)
                if (count.second && count.first != target_inventory[work][description])
                    finding(result["work"]["differences"],
                            {
                                {   "reason",         "work_multiplicity_differs" },
                                {  "work_id",                          work.first },
                                {     "rank",                         work.second },
                                {     "work",                         description },
                                { "observed",                         count.first },
                                {   "target", target_inventory[work][description] }
                    });
            for (const auto & node : nodes)
                if (!node.used && !node.ambiguous)
                    finding(result["work"]["differences"],
                            {
                                {      "reason", "observed_work_not_in_target" },
                                {     "work_id",                    work.first },
                                {        "rank",                   work.second },
                                { "source_span",                     node.span },
                                {        "work",              node.description }
                    });
        }
        for (const auto & [work, units] : collectives)
            for (const auto & unit : units)
                if (!mismatched_states.contains(work) && !unit.operation_used)
                    finding(result["work"]["differences"],
                            {
                                {  "reason", "observed_collective_interface_not_in_target" },
                                { "work_id",                                    work.first },
                                {    "rank",                                   work.second },
                                {   "scope",                        unit.observation.scope },
                                {    "work",              unit.observation.work.describe() }
                    });

        for (const auto & port : record.at("ports")) {
            const WorkRank work{ port.at("work_id"), port.at("rank") };
            const auto matched = matches.find(work);
            if (matched == matches.end()) continue;
            const auto cut = cuts.find(matched->second);
            if (cut == cuts.end()) continue;
            const auto kind = contract.block(calls_[matched->second].step).exits;
            for (const auto candidate : kind)
                if (contract.port_name(candidate) == port.at("port").get_ref<const std::string &>()) {
                    if (const auto point = cut->second->ports.find(candidate); point != cut->second->ports.end()) {
                        bind_boundary(port.at("boundary"), point->second);
                        if (const auto frontier = boundary_matches.find(port.at("boundary")); frontier != boundary_matches.end())
                            published_ports.emplace(std::tuple{ work.first, work.second, port.at("port").get<std::string>() }, frontier->second);
                    }
                    else
                        finding(result["causal"]["unobserved"],
                                {
                                    {  "reason", "native_port_not_observed" },
                                    { "work_id",                 work.first },
                                    {    "rank",                work.second },
                                    {    "port",            port.at("port") }
                        });
                }
        }
        std::map<WorkRank, std::map<std::size_t, const Json *>> input_ready;
        for (const auto & input : record.at("request_ready")) {
            const WorkRank work{ input.at("work_id"), input.at("rank") };
            const auto slot = input.at("input_slot").get<std::size_t>();
            if (!input_ready[work].emplace(slot, &input).second)
                finding(result["effect"]["differences"],
                        {
                            {     "reason", "target_request_ready_not_unique" },
                            {    "work_id",                        work.first },
                            {       "rank",                       work.second },
                            { "input_slot",                              slot }
                });
        }
        for (const auto & [work, call] : matches) {
            if (calls_[call].step != StepKind::Schedule || mismatched_states.contains(work)) continue;
            const auto native = semantics_.incoming_request_ready(call);
            if (!native) {
                finding(result["causal"]["unobserved"],
                        {
                            {  "reason", "native_request_input_not_observed" },
                            { "work_id",                          work.first },
                            {    "rank",                         work.second }
                });
                continue;
            }
            const auto invocation = std::ranges::find_if(record.at("invocations"), [&](const Json & entry) { return entry.at("work_id") == work.first; });
            const auto & incoming = invocation->at("incoming_requests");
            std::set<std::size_t> observed;
            for (const auto & [slot, point] : *native) {
                observed.insert(slot);
                const auto target = input_ready[work].find(slot);
                if (target == input_ready[work].end()) {
                    finding(result["effect"]["differences"],
                            {
                                {     "reason", "observed_request_ready_not_in_target" },
                                {    "work_id",                             work.first },
                                {       "rank",                            work.second },
                                { "input_slot",                                   slot }
                    });
                    continue;
                }
                if (slot >= incoming.size() || target->second->at("request_id") != incoming.at(slot).at("request_id")) {
                    finding(result["effect"]["differences"],
                            {
                                {     "reason", "request_ready_identity_differs" },
                                {    "work_id",                       work.first },
                                {       "rank",                      work.second },
                                { "input_slot",                             slot }
                    });
                    continue;
                }
                bind_boundary(target->second->at("boundary"), point);
            }
            for (const auto & [slot, input] : input_ready[work])
                if (!observed.contains(slot))
                    finding(result["effect"]["differences"],
                            {
                                {     "reason", "target_request_ready_not_observed" },
                                {    "work_id",                          work.first },
                                {       "rank",                         work.second },
                                { "input_slot",                                slot }
                    });
        }
        for (const auto & [work, call] : matches) {
            if (calls_[call].step != StepKind::Publish || calls_[call].requests.empty()) continue;
            const auto native = semantics_.output(call);
            if (!native) {
                finding(result["causal"]["unobserved"],
                        {
                            {  "reason", "native_request_output_not_observed" },
                            { "work_id",                           work.first },
                            {    "rank",                          work.second }
                });
                continue;
            }
            std::vector<std::size_t> output_scopes;
            for (std::size_t index = 0; index < record.at("nodes").size(); ++index) {
                const auto & node = record.at("nodes")[index];
                if (node.at("work_id") != work.first || node.at("rank") != work.second || node.at("role") != "annotation"
                    || node.at("kind") != SourceGraph::span_name(SourceSpanKind::CpuObservation))
                    continue;
                const auto & scope = node.at("logical_scope");
                if (!scope.is_null() && scope.at("binding") == "source_scope" && !scope.at("path").is_null() && !native->scope.at("path").is_null()
                    && compatible_scope(scope, native->scope))
                    output_scopes.push_back(index);
            }
            const Json * output_scope = nullptr;
            if (output_scopes.size() == 1) {
                output_scope = &record.at("nodes")[output_scopes.front()];
                // Anchor the observed service independently of the claimed
                // output endpoint, so an entry point cannot masquerade as exit.
                bind_boundary(output_scope->at("begin"), native->begin);
                bind_boundary(output_scope->at("end"), native->end);
            }
            else
                finding(result["causal"]["unobserved"],
                        {
                            {                  "reason", "request_output_scope_not_unique" },
                            {                 "work_id",                        work.first },
                            {                    "rank",                       work.second },
                            { "target_scope_candidates",              output_scopes.size() }
                });
            const auto invocation = std::ranges::find_if(record.at("invocations"), [&](const Json & entry) { return entry.at("work_id") == work.first; });
            auto expected = Json::array(), observed_output = Json::array();
            for (const auto & output : record.at("outputs")) {
                if (output.at("work_id") != work.first || output.at("rank") != work.second) continue;
                const auto & request = invocation->at("requests").at(output.at("request_slot").get<std::size_t>());
                expected.push_back({
                    {        "request_id",               request.at("id") },
                    {            "tokens",            output.at("tokens") },
                    { "cumulative_tokens", output.at("cumulative_tokens") },
                    {          "finishes",          output.at("finishes") }
                });
                if (output_scope && output.at("boundary") != output_scope->at("end"))
                    finding(result["causal"]["differences"],
                            {
                                {        "reason", "request_output_endpoint_differs" },
                                {       "work_id",                        work.first },
                                {          "rank",                       work.second },
                                {      "boundary",             output.at("boundary") },
                                { "expected_exit",           output_scope->at("end") }
                    });
            }
            for (const auto & output : native->requests)
                observed_output.push_back({
                    {        "request_id",        output.request_id },
                    {            "tokens",            output.tokens },
                    { "cumulative_tokens", output.cumulative_tokens },
                    {          "finishes",          output.finishes }
                });
            if (expected != observed_output)
                finding(result["effect"]["differences"],
                        {
                            {   "reason", "request_output_differs" },
                            {  "work_id",               work.first },
                            {     "rank",              work.second },
                            {   "target",                 expected },
                            { "observed",          observed_output }
                });
        }
        for (std::size_t point = 0; point < record.at("boundaries").size(); ++point) {
            const auto & boundary = record.at("boundaries")[point];
            if (boundary.contains("resource_predecessor")) {
                const auto & peer = boundary.at("resource_predecessor");
                const auto found = endpoints.find({ peer.at("work_id"), peer.at("rank"), peer.at("boundary") });
                if (found != endpoints.end()) bind_frontier(point, found->second);
            }
            else if (boundary.contains("port_predecessor")) {
                const auto & peer = boundary.at("port_predecessor");
                const auto found = published_ports.find({ peer.at("work_id"), peer.at("rank"), peer.at("port").get<std::string>() });
                if (found != published_ports.end()) bind_frontier(point, found->second);
            }
        }
        std::map<std::size_t, std::vector<std::size_t>> native_anchors, target_anchors, target_frontiers;
        for (const auto & [target, frontier] : boundary_matches) {
            for (const auto native : frontier) native_anchors[native].push_back(target);
            auto points = target_completions.normalize({ target });
            for (const auto point : points) target_anchors[point].push_back(target);
            target_frontiers.emplace(target, std::move(points));
        }
        // Omitted observer/background work has no price here, but paths through
        // it still constrain matched interfaces. Project the complete native DAG.
        FrontierProjection native_projection(native_paths, native_edges, native_order, native_anchors, boundary_matches);
        FrontierProjection target_projection(target_paths, edges, target_order, target_anchors, target_frontiers);
        for (const auto & [from, frontier] : boundary_matches) {
            native_projection.from(from);
            target_projection.from(from);
            for (const auto to : native_projection.successors()) {
                result["causal"]["checked_paths"] = result["causal"]["checked_paths"].get<std::size_t>() + 1;
                if (!target_projection.precedes(to))
                    finding(result["causal"]["differences"],
                            {
                                { "reason", "observed_dependency_missing_in_target" },
                                {  "batch",                      record.at("batch") },
                                { "source",                                    from },
                                { "target",                                      to }
                    });
            }
            for (const auto to : target_projection.successors()) {
                if (!native_projection.precedes(to))
                    finding(result["causal"]["unobserved"],
                            {
                                { "reason", "target_constraint_not_independently_observed" },
                                {  "batch",                             record.at("batch") },
                                { "source",                                           from },
                                { "target",                                             to }
                    });
            }
        }
        if (!ambiguous_boundaries.empty())
            finding(result["causal"]["unobserved"],
                    {
                        {         "reason", "shared_boundary_mapping_ambiguous" },
                        {          "batch",                  record.at("batch") },
                        { "boundary_count",         ambiguous_boundaries.size() }
            });
        for (const auto & instance : record.at("instances")) {
            const auto & points = instance.at("boundaries");
            for (std::size_t local = 0; local < points.size(); ++local)
                if (const auto found = boundary_matches.find(points[local]); found != boundary_matches.end())
                    endpoints.emplace(Endpoint{ instance.at("work_id"), instance.at("rank"), local }, found->second);
        }
    }
    if (batches == 0)
        return {
            { "status",                    "not_comparable" },
            { "reason", "fixed_target_graph_has_no_batches" }
        };
    for (const auto & [key, remaining] : actions)
        for (const auto index : remaining)
            finding(result["control"]["differences"],
                    {
                        { "reason", "observed_action_not_in_target" },
                        {   "call",          calls_[index].identity },
                        {   "rank",                        key.rank },
                        {   "step",   contract.block(key.step).name },
                        {  "phase",  contract.phase_name(key.phase) }
            });

    for (const auto category : { "control", "work", "causal", "effect", "state_coverage" })
        if (result.at(category).at("differences").at("total") != 0) result["status"] = "different";
    if (result.at("status") == "compared"
        && (result.at("work").at("unobserved").at("total") != 0 || result.at("causal").at("unobserved").at("total") != 0
            || result.at("state_coverage").at("unobserved").at("total") != 0 || result.at("coverage").at("status") != "observed"
            || result.at("coverage").at("dependency_coverage") != "complete" || !subgraphs_.gaps.empty()))
        result["status"] = "partial";
    return result;
}

} // namespace markov
