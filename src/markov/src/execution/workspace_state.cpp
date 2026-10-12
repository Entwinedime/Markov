#include "markov/execution/workspace_state.hpp"

#include <cstddef>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

namespace markov {

WorkspaceState::WorkspaceState(std::span<const TemplateWorkspacePool> pools) {
    for (const auto & pool : pools) capacities_.emplace(pool.resource, pool.initial_capacity_bytes);
}

std::expected<void, std::string> WorkspaceState::advance(TemplateInstance & instance, const TemplateBank & templates, const BatchInput & input) {
    if (instance.workspace_uses.empty()) return {};

    const auto & definition = *instance.definition;
    const auto & topology = instance.topology;
    const auto successors = topology.successors(definition);
    const auto count = successors.size();
    std::vector<std::size_t> indegree(count);
    for (const auto & next : successors)
        for (const auto point : next) ++indegree[point];

    std::queue<std::size_t> ready;
    for (std::size_t point = 0; point < count; ++point)
        if (indegree[point] == 0) ready.push(point);
    std::map<std::size_t, std::vector<std::size_t>> uses;
    for (std::size_t index = 0; index < instance.workspace_uses.size(); ++index) uses[instance.workspace_uses[index].begin].push_back(index);
    std::vector<std::size_t> ordered;
    std::size_t visited = 0;
    while (!ready.empty()) {
        const auto point = ready.front();
        ready.pop();
        ++visited;
        if (const auto found = uses.find(point); found != uses.end()) ordered.insert(ordered.end(), found->second.begin(), found->second.end());
        for (const auto next : successors[point])
            if (--indegree[next] == 0) ready.push(next);
    }
    if (visited != count) return std::unexpected("workspace_dependency_cycle");

    std::map<TemplateResource, std::size_t> previous;
    std::vector<TemplateOperationRewrite> replacements;
    for (const auto index : ordered) {
        const auto & target = instance.workspace_uses[index];
        const auto & observed = target.definition->workspace_uses[target.source_use];
        const auto & pool = templates.workspace_pool(*target.definition, observed.pool);
        const auto capacity = capacities_.find(pool.resource);
        if (capacity == capacities_.end()) return std::unexpected("workspace_initial_resource_not_qualified");

        // A topological enumeration alone does not prove an order between
        // independent uses. Require an actual path on each shared pool.
        if (const auto prior = previous.find(pool.resource); prior != previous.end()) {
            std::vector<bool> reached(count);
            std::vector<std::size_t> pending{ prior->second };
            reached[prior->second] = true;
            while (!pending.empty() && !reached[target.begin]) {
                const auto point = pending.back();
                pending.pop_back();
                for (const auto next : successors[point]) {
                    if (reached[next]) continue;
                    reached[next] = true;
                    pending.push_back(next);
                }
            }
            if (!reached[target.begin]) return std::unexpected("workspace_use_order_not_qualified");
        }
        previous[pool.resource] = target.end;

        auto branch = WorkspaceBranch::Release;
        std::uint64_t next_capacity = 0;
        if (observed.branch != WorkspaceBranch::Release) {
            if (!target.demand) return std::unexpected(observed.demand.error());
            const auto demand = target.demand->bytes;
            const auto alignment = pool.allocation_alignment_bytes;
            if (demand > std::numeric_limits<std::uint64_t>::max() - pool.allocation_padding_bytes - alignment)
                return std::unexpected("workspace_capacity_out_of_range");

            const auto required = demand + pool.allocation_padding_bytes;
            if (required <= capacity->second) branch = WorkspaceBranch::Reuse;
            else if (capacity->second == 0) branch = WorkspaceBranch::Allocate;
            else branch = WorkspaceBranch::Grow;
            next_capacity = branch == WorkspaceBranch::Reuse ? capacity->second : (required + alignment - 1) / alignment * alignment;
        }
        // Replace allocator work only. The operation keeps its target demand
        // and the common cut preserves worker ports and synchronization queries.
        if (branch != observed.branch) {
            auto replacement = templates.plan_workspace_replacement(instance, target, branch, input);
            if (!replacement) return std::unexpected(replacement.error());
            replacements.push_back(std::move(*replacement));
        }
        capacity->second = next_capacity;
    }
    if (!replacements.empty()) {
        const auto applied = instance.topology.apply_operations(definition, replacements);
        if (!applied) return std::unexpected(applied.error());
        return instance.bind_workspace_uses(input);
    }
    return {};
}

} // namespace markov
