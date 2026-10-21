#include "markov/production/block_extractor.hpp"

#include "markov/production/source_bindings.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace markov {

BlockExtractor::BlockExtractor(const SourceGraph & graph, std::span<const SourceCall> calls, const SourceBindings & bindings)
    : graph_(graph),
      calls_(calls),
      bindings_(bindings) {}

std::vector<RegionSubgraph> BlockExtractor::extract_regions() const {
    // Regions reference existing spans and cut boundaries. They do not create
    // executable work, completion barriers, or another CPU charge.
    std::map<BoundaryId, std::set<std::size_t>> users;
    for (SourceSpanId identity = 0; identity < graph_.spans().size(); ++identity) {
        const auto & span = graph_.spans()[identity];
        auto scope = span.scope_call;
        while (scope && calls_[*scope].is_annotation()) {
            users[span.begin].insert(*scope);
            users[span.end].insert(*scope);
            scope = calls_[*scope].parent;
        }
    }

    // Completion joins have no service span. They belong to a region only when
    // every completed span belongs to it; a cross-region join stays external.
    // SourceGraph::complete connects span ends directly, so no recursive closure
    // or temporal containment is needed to establish this membership.
    const std::set<std::size_t> empty;
    std::map<BoundaryId, std::set<std::size_t>> completion_users;
    for (const auto & edge : graph_.edges()) {
        if (edge.relation != SourceRelation::CompletionJoin) continue;
        const auto source = users.find(edge.source);
        const auto & regions = source == users.end() ? empty : source->second;
        auto [position, inserted] = completion_users.try_emplace(edge.target, regions);
        if (!inserted) std::erase_if(position->second, [&](std::size_t region) { return !regions.contains(region); });
    }
    for (auto & [boundary, regions] : completion_users) users[boundary].insert(regions.begin(), regions.end());

    std::vector<std::set<BoundaryId>> inputs(calls_.size());
    std::vector<std::set<BoundaryId>> outputs(calls_.size());
    for (std::size_t index = 0; index < graph_.edges().size(); ++index) {
        const auto & edge = graph_.edges()[index];
        const auto source = users.find(edge.source);
        const auto target = users.find(edge.target);
        const auto & sources = source == users.end() ? empty : source->second;
        const auto & targets = target == users.end() ? empty : target->second;
        for (const auto region : sources) {
            if (!targets.contains(region)) outputs[region].insert(edge.source);
        }
        for (const auto region : targets) {
            if (!sources.contains(region)) inputs[region].insert(edge.target);
        }
    }

    std::vector<RegionSubgraph> regions;
    for (std::size_t index = 0; index < calls_.size(); ++index) {
        const auto & call = calls_[index];
        if (!call.is_annotation()) continue;
        regions.push_back({ index, call.serving_parent(calls_).value(), std::move(inputs[index]), std::move(outputs[index]) });
    }
    return regions;
}

SourceSubgraphs BlockExtractor::extract() const {
    SourceSubgraphs result;
    result.regions = extract_regions();
    std::vector<bool> markers(graph_.spans().size());
    std::vector<bool> cuts(graph_.boundaries().size());
    for (const auto & call : calls_)
        if (call.marker) markers[*call.marker] = true;

    // Native operation windows and semantic ports survive as boundaries. The
    // profiler's nested function envelopes are provenance, not executable work.
    for (const auto & [span, operation] : graph_.operations()) {
        const auto & host = graph_.spans()[operation.host_span];
        cuts[host.begin] = cuts[host.end] = true;
        for (const auto identity : operation.worker_spans) {
            const auto & worker = graph_.spans()[identity];
            cuts[worker.begin] = cuts[worker.end] = true;
        }
    }
    for (const auto & region : result.regions) {
        for (const auto point : region.inputs) cuts[point] = true;
        for (const auto point : region.outputs) cuts[point] = true;
        if (const auto marker = calls_[region.call].marker) {
            const auto & span = graph_.spans()[*marker];
            cuts[span.begin] = cuts[span.end] = true;
        }
    }
    for (const auto & edge : graph_.edges()) {
        if (edge.relation != SourceRelation::ThreadOrder && edge.relation != SourceRelation::IntervalExtent) {
            cuts[edge.source] = true;
            cuts[edge.target] = true;
        }
    }

    std::vector<std::vector<std::size_t>> users(graph_.boundaries().size());
    std::vector<std::vector<SourceSpanId>> members(calls_.size());
    std::vector<std::vector<BoundaryId>> logical_members(calls_.size());
    std::set<std::pair<BoundaryId, BoundaryId>> extents;
    for (SourceSpanId identity = 0; identity < graph_.spans().size(); ++identity) {
        const auto & span = graph_.spans()[identity];
        if (!span.owner_call) continue;

        // Omit pure CPU provenance. Resource observations carry state effects
        // and must remain members through sharing and target transformations.
        const bool retained = span.kind != SourceSpanKind::CpuObservation || markers[identity];
        if (retained) {
            members[*span.owner_call].push_back(identity);
            extents.emplace(span.begin, span.end);
        }
        for (const auto point : { span.begin, span.end }) {
            if (retained || cuts[point]) {
                users[point].push_back(*span.owner_call);
                if (!retained) logical_members[*span.owner_call].push_back(point);
            }
        }
    }

    for (BoundaryId identity = 0; identity < graph_.boundaries().size(); ++identity) {
        const auto & point = graph_.boundaries()[identity];
        if (point.owner_call) {
            users[identity].push_back(*point.owner_call);
            logical_members[*point.owner_call].push_back(identity);
        }
    }

    for (auto & owners : users) {
        std::sort(owners.begin(), owners.end());
        owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
    }
    for (auto & points : logical_members) {
        std::sort(points.begin(), points.end());
        points.erase(std::unique(points.begin(), points.end()), points.end());
    }

    // Project recorded thread paths through omitted annotations. Paths whose
    // endpoint owners agree are internal, so their omitted points must not also
    // create dangling external connections for the same ordering constraint.
    const auto absent = graph_.boundaries().size();
    std::vector<BoundaryId> thread_next(absent, absent);
    for (const auto & edge : graph_.edges())
        if (edge.relation == SourceRelation::ThreadOrder) thread_next[edge.source] = edge.target;

    std::vector<bool> internal_thread_points(absent);
    std::vector<std::vector<std::array<BoundaryId, 2>>> internal_thread_paths(calls_.size());
    for (BoundaryId begin = 0; begin < absent; ++begin) {
        if (users[begin].empty()) continue;
        auto end = thread_next[begin];
        if (end == absent || !users[end].empty()) continue;

        while (end != absent && users[end].empty()) end = thread_next[end];
        if (end == absent) continue;
        for (const auto owner : users[begin])
            if (std::binary_search(users[end].begin(), users[end].end(), owner)) internal_thread_paths[owner].push_back({ begin, end });

        if (users[begin] == users[end])
            for (auto point = thread_next[begin]; point != end; point = thread_next[point]) internal_thread_points[point] = true;
    }

    // Shared owners retain internal edges; ownership changes expose template boundaries.
    std::vector<std::vector<std::size_t>> internal_edges(calls_.size());
    for (std::size_t index = 0; index < graph_.edges().size(); ++index) {
        const auto & edge = graph_.edges()[index];
        if (edge.relation == SourceRelation::IntervalExtent && !extents.contains({ edge.source, edge.target })) continue;
        if (edge.relation == SourceRelation::ThreadOrder && (internal_thread_points[edge.source] || internal_thread_points[edge.target])) continue;

        const auto & sources = users[edge.source];
        const auto & targets = users[edge.target];
        if (sources.empty() && targets.empty()) continue;

        for (const auto owner : sources) {
            if (std::binary_search(targets.begin(), targets.end(), owner)) internal_edges[owner].push_back(index);
        }

        if (sources == targets) continue;
        result.crossings.push_back({ index, sources, targets });
    }

    const ServingContract contract;
    for (std::size_t index = 0; index < calls_.size(); ++index) {
        const auto & call = calls_[index];
        if (call.is_annotation()) continue;

        auto ports = bindings_.ports(graph_, calls_, index);
        for (const auto port : contract.block(call.step).exits) {
            if (!ports.contains(port)) {
                result.gaps.push_back({
                    {   "kind",             "observation" },
                    { "reason", "block_port_not_observed" },
                    {   "call",             call.identity },
                    {   "port",  contract.port_name(port) }
                });
            }
        }

        result.blocks.push_back({ index,
                                  std::move(members[index]),
                                  std::move(logical_members[index]),
                                  std::move(internal_thread_paths[index]),
                                  std::move(internal_edges[index]),
                                  std::move(ports) });
    }
    return result;
}

} // namespace markov
