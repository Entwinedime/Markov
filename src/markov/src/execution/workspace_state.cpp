#include "markov/execution/workspace_state.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <queue>
#include <string_view>
#include <utility>
#include <vector>

namespace markov {
namespace {

std::string_view branch_name(WorkspaceBranch branch) {
    switch (branch) {
    case WorkspaceBranch::Reuse:
        return "reuse";
    case WorkspaceBranch::Allocate:
        return "allocate";
    case WorkspaceBranch::Grow:
        return "grow";
    case WorkspaceBranch::Release:
        return "release";
    }
    std::unreachable();
}

} // namespace

WorkspaceState::WorkspaceState(std::span<const TemplateWorkspacePool> pools) {
    for (const auto & pool : pools) capacities_.emplace(pool.resource, pool.initial_capacity_bytes);
}

std::expected<void, std::string> WorkspaceState::advance(TemplateInstance & instance, const TemplateBank & templates, const BatchInput & input,
                                                         bool diagnostics) {
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
        auto & target = instance.workspace_uses[index];
        const auto & observed = target.definition->workspace_uses[target.source_use];
        const auto & pool = templates.workspace_pool(*target.definition, observed.pool);
        const auto resource = instance.resource(pool.resource);
        const auto capacity = capacities_.find(resource);
        if (capacity == capacities_.end()) return std::unexpected("workspace_initial_resource_not_qualified");

        // A topological enumeration alone does not prove an order between
        // independent uses. Require an actual path on each shared pool.
        if (const auto prior = previous.find(resource); prior != previous.end()) {
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
        previous[resource] = target.end;

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
        if (diagnostics) {
            target.effect = {
                {                            "pool",        resource.describe()                                                    },
                {                    "demand_bytes", target.demand ? nlohmann::json(target.demand->bytes) : nlohmann::json(nullptr) },
                {           "capacity_before_bytes",                                                               capacity->second },
                {            "capacity_after_bytes",                                                                  next_capacity },
                {                          "branch",                                                            branch_name(branch) },
                { "requires_device_synchronization",
                 branch == observed.branch ? nlohmann::json(observed.requires_device_synchronization) : nlohmann::json(nullptr)    }
            };
        }
        capacity->second = next_capacity;
    }
    if (!replacements.empty()) {
        const auto applied = instance.topology.apply_operations(definition, replacements);
        if (!applied) return std::unexpected(applied.error());
        // Rebinding resolves the actual donor uses. Keep diagnostics only across
        // the same qualified allocator cut and pool; donor state is not replayed.
        auto recorded = diagnostics ? std::move(instance.workspace_uses) : std::vector<TemplateWorkspaceUseInstance>{};
        const auto bound = instance.bind_workspace_uses(input);
        if (!bound) return bound;
        if (diagnostics) {
            for (auto & target : instance.workspace_uses) {
                const auto same_cut = [&](const auto & prior) { return prior.begin == target.begin && prior.end == target.end; };
                if (std::ranges::count_if(recorded, same_cut) != 1) continue;
                auto & prior = *std::ranges::find_if(recorded, same_cut);
                const auto & selected = target.definition->workspace_uses[target.source_use];
                const auto & original = prior.definition->workspace_uses[prior.source_use];
                const auto pool = instance.resource(templates.workspace_pool(*target.definition, selected.pool).resource);
                const auto previous_pool = instance.resource(templates.workspace_pool(*prior.definition, original.pool).resource);
                if (pool != previous_pool || prior.effect.is_null()) continue;
                target.effect = std::move(prior.effect);
                target.effect["requires_device_synchronization"] = nullptr;
                if (target.effect.at("branch") == branch_name(selected.branch))
                    target.effect["requires_device_synchronization"] = selected.requires_device_synchronization;
            }
        }
    }
    return {};
}

} // namespace markov
