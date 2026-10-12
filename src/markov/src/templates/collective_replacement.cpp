#include "markov/templates/templates.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace markov {
namespace {

struct CollectiveMember {
    const TemplateDefinition * definition;
    const TemplateCollective * collective;
};

class CollectiveGroup {
public:
    std::vector<CollectiveMember> members;

    static std::expected<std::vector<CollectiveGroup>, std::string> observe(std::span<const TemplateDefinition * const> definitions, std::uint32_t rank_count) {
        std::vector<CollectiveMember> entries;
        std::map<BoundaryId, std::size_t> owners;
        for (const auto * definition : definitions) {
            for (const auto & collective : definition->collectives) {
                const auto member = entries.size();
                entries.push_back({ definition, &collective });
                for (const auto index : collective.task_nodes) {
                    const auto & node = definition->graph[index];
                    owners.emplace(definition->boundary_observations[node.begin].source_boundary, member);
                    owners.emplace(definition->boundary_observations[node.end].source_boundary, member);
                }
            }
        }

        // Native notifications, rather than rank-local operation order, identify a group.
        std::vector<std::set<std::size_t>> peers(entries.size());
        for (const auto * definition : definitions) {
            for (std::size_t index = 0; index < definition->graph.connection_count(); ++index) {
                const auto connection = definition->connection(index);
                if (connection.relation != SourceRelation::NativeNotifySignal) continue;
                const auto local = owners.find(definition->boundary_observations[connection.local_boundary].source_boundary);
                if (local == owners.end()) continue;
                const auto remote = owners.find(connection.remote_source_boundary);
                if (remote == owners.end()) return std::unexpected("collective_notification_peer_not_observed");
                const auto remote_call = entries[remote->second].definition->source_call;
                if (std::ranges::find(connection.remote_source_calls, remote_call) == connection.remote_source_calls.end())
                    return std::unexpected("collective_notification_owner_not_observed");

                peers[local->second].insert(remote->second);
                peers[remote->second].insert(local->second);
            }
        }

        std::vector<CollectiveGroup> groups;
        std::vector<bool> visited(entries.size());
        for (std::size_t first = 0; first < entries.size(); ++first) {
            if (visited[first]) continue;
            std::vector<std::size_t> pending{ first };
            visited[first] = true;
            for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
                for (const auto peer : peers[pending[cursor]]) {
                    if (visited[peer]) continue;
                    visited[peer] = true;
                    pending.push_back(peer);
                }
            }

            std::set<std::uint32_t> ranks;
            CollectiveGroup group;
            for (const auto index : pending) {
                const auto & member = entries[index];
                if (!ranks.insert(member.definition->rank).second || pending.size() != rank_count) return std::unexpected("collective_rank_group_incomplete");
                group.members.push_back(member);
            }
            std::ranges::sort(group.members, {}, [](const CollectiveMember & member) { return member.definition->rank; });
            groups.push_back(std::move(group));
        }
        return groups;
    }

    std::expected<std::vector<std::vector<SourceCommunicationTask>>, std::string> work(const BatchInput & target) const {
        std::vector<std::vector<SourceCommunicationTask>> result;
        std::optional<std::uint64_t> count;
        std::optional<CommunicationOperation> operation;
        for (const auto & member : members) {
            const auto & collective = *member.collective;
            const auto & rule = member.definition->graph.collective_rule(collective.envelope_node);
            if (!rule.count) return std::unexpected("collective_parameter_relation_missing");
            if (!rule.work) return std::unexpected(rule.work.error());
            const auto requested = rule.count->count(target);
            if ((count && *count != requested) || (operation && *operation != rule.work->operation())) return std::unexpected("collective_rank_work_disagrees");
            count = requested;
            operation = rule.work->operation();
        }

        for (const auto & member : members) {
            const auto & rule = member.definition->graph.collective_rule(member.collective->envelope_node);
            auto tasks = rule.work->evaluate(member.collective->observation.tasks, *count);
            if (!tasks) return std::unexpected(tasks.error());
            result.push_back(std::move(*tasks));
        }
        return result;
    }

    bool compatible(const CollectiveGroup & donor) const {
        if (members.size() != donor.members.size()) return false;
        for (std::size_t index = 0; index < members.size(); ++index) {
            const auto & source = *members[index].collective;
            const auto & other = *donor.members[index].collective;
            const auto & source_rule = members[index].definition->graph.collective_rule(source.envelope_node);
            const auto & other_rule = donor.members[index].definition->graph.collective_rule(other.envelope_node);
            if (!source_rule.count || !other_rule.count || !source_rule.work || !other_rule.work) return false;
            if (source_rule.count->rows != other_rule.count->rows || source_rule.count->width != other_rule.count->width
                || source_rule.work->operation() != other_rule.work->operation() || source.observation.data_type != other.observation.data_type)
                return false;
        }
        return true;
    }
};

class CollectiveReplacement {
public:
    TemplateOperationRewrite rewrite;
    std::map<std::size_t, std::size_t> boundary_mapping;
    std::vector<std::size_t> removed_connections;
    std::vector<std::size_t> donor_connections;

    static std::expected<CollectiveReplacement, std::string> plan(const CollectiveMember & recipient, const CollectiveMember & donor,
                                                                  std::span<const SourceCommunicationTask> work) {
        const auto & source = *recipient.definition;
        const auto & from = *donor.definition;
        const auto operation = std::ranges::find_if(source.operations, [&recipient](const TemplateOperation & item) {
            return item.collective_spans.size() == 1 && item.collective_spans.front() == recipient.collective->observation.span;
        });
        const auto donor_operation = std::ranges::find_if(from.operations, [&donor](const TemplateOperation & item) {
            return item.collective_spans.size() == 1 && item.collective_spans.front() == donor.collective->observation.span;
        });
        if (operation == source.operations.end() || donor_operation == from.operations.end()) return std::unexpected("collective_operation_not_observed");
        const auto region = source.operation_region(*operation);
        const auto donor_region = from.operation_region(*donor_operation);
        const auto ports = source.collective_operation_ports(*operation, region);
        const auto donor_ports = from.collective_operation_ports(*donor_operation, donor_region);
        if (!ports) return std::unexpected(ports.error());
        if (!donor_ports) return std::unexpected(donor_ports.error());
        if (ports->stream_predecessor.has_value() != donor_ports->stream_predecessor.has_value()
            || operation->workers.size() != donor_operation->workers.size())
            return std::unexpected("collective_operation_interface_disagrees");

        std::map<std::size_t, std::size_t> mapping{
            {       donor_operation->host.begin,       operation->host.begin },
            {         donor_operation->host.end,         operation->host.end },
            {    donor_ports->stream_completion,    ports->stream_completion },
            { donor_ports->enclosing_completion, ports->enclosing_completion }
        };
        if (ports->stream_predecessor) mapping.emplace(*donor_ports->stream_predecessor, *ports->stream_predecessor);
        std::map<std::size_t, std::size_t> lanes{
            { from.graph.boundary(donor_operation->host.begin).lane, source.graph.boundary(operation->host.begin).lane }
        };
        for (const auto & worker : donor_operation->workers) {
            const auto & lane = from.lanes[from.graph.boundary(worker.begin).lane].source;
            const auto found = std::ranges::find_if(operation->workers, [&source, &lane](const TemplateOperationWindow & window) {
                const auto & candidate = source.lanes[source.graph.boundary(window.begin).lane].source;
                return candidate.domain == lane.domain && candidate.native_id == lane.native_id;
            });
            if (found == operation->workers.end()) return std::unexpected("collective_worker_resource_not_matched");
            mapping.emplace(worker.begin, found->begin);
            mapping.emplace(worker.end, found->end);
            lanes.emplace(from.graph.boundary(worker.begin).lane, source.graph.boundary(found->begin).lane);
        }

        // Physical streams keep their native resource. Annotation and task-join
        // lanes are operation-local roles, not identities shared with the donor.
        for (const auto point : donor_region.boundaries) {
            const auto lane_index = from.graph.boundary(point).lane;
            if (lanes.contains(lane_index)) continue;
            const auto & lane = from.lanes[lane_index].source;
            std::set<std::size_t> candidates;
            for (const auto target : region.boundaries) {
                const auto index = source.graph.boundary(target).lane;
                const auto & other = source.lanes[index].source;
                if (other.domain == lane.domain && (lane.domain != SourceDomain::DeviceStream || other.native_id == lane.native_id)) candidates.insert(index);
            }
            if (candidates.size() != 1) return std::unexpected("collective_lane_role_not_matched");
            lanes.emplace(lane_index, *candidates.begin());
        }

        CollectiveReplacement result;
        result.rewrite.nodes = region.nodes;
        result.rewrite.dependencies = region.internal_dependencies;
        auto imported = donor_region.internal_dependencies;
        for (const auto index : region.incoming_dependencies)
            if (source.graph.dependencies()[index].relation == SourceRelation::StreamOrder) result.rewrite.dependencies.push_back(index);
        for (const auto index : region.outgoing_dependencies)
            if (source.graph.dependencies()[index].relation == SourceRelation::CompletionJoin) result.rewrite.dependencies.push_back(index);
        for (const auto index : donor_region.incoming_dependencies)
            if (from.graph.dependencies()[index].relation == SourceRelation::StreamOrder) imported.push_back(index);
        for (const auto index : donor_region.outgoing_dependencies)
            if (from.graph.dependencies()[index].relation == SourceRelation::CompletionJoin) imported.push_back(index);

        result.boundary_mapping = result.rewrite.insert_subgraph(from, source, donor_region.nodes, imported, std::move(mapping), lanes);
        std::map<SourceSpanId, const SourceCommunicationTask *> tasks;
        for (const auto & task : work) tasks.emplace(task.span, &task);
        for (auto & inserted : result.rewrite.inserted_nodes) {
            if (inserted.observation().communication) inserted.communication_override = *tasks.at(inserted.observation().source_span);
            if (inserted.observation().kernel) return std::unexpected("collective_region_contains_compute");
            if (inserted.node.kind == SourceSpanKind::Dma && !inserted.observation().communication)
                return std::unexpected("collective_region_contains_unbound_transfer");
        }
        result.removed_connections = ports->notification_connections;
        result.donor_connections = donor_ports->notification_connections;
        return result;
    }
};

} // namespace

std::expected<void, TemplateGap> TemplateBank::parameterize_communications(std::span<TemplateInstance> instances, const InvokeStep & invocation) const {
    std::vector<const TemplateDefinition *> definitions;
    for (const auto & instance : instances) definitions.push_back(instance.definition);
    const auto groups = CollectiveGroup::observe(definitions, source_.ranks);
    if (!groups) return std::unexpected(TemplateGap{ TemplateGapKind::Observation, groups.error(), invocation.step, invocation.input.phase, 0 });

    std::optional<std::vector<CollectiveGroup>> donors;
    for (const auto & group : *groups) {
        auto work = group.work(invocation.input);
        if (work) {
            for (std::size_t index = 0; index < group.members.size(); ++index) {
                auto & instance = instances[group.members[index].definition->rank];
                for (auto & task : (*work)[index]) instance.communication_overrides.emplace(task.span, std::move(task));
            }
            continue;
        }

        // Only existing, producer-qualified groups can supply another topology.
        // No native gap string controls selection and no observation is requested.
        if (!donors) {
            donors.emplace();
            std::map<std::optional<std::uint64_t>, std::vector<const TemplateDefinition *>> cohorts;
            for (const auto & [key, candidates] : source_.definitions) {
                if (std::get<0>(key) != invocation.step || std::get<1>(key) != invocation.input.phase) continue;
                for (const auto & definition : candidates) cohorts[definition.source_batch].push_back(&definition);
            }
            for (const auto & [batch, cohort] : cohorts) {
                auto observed = CollectiveGroup::observe(cohort, source_.ranks);
                if (observed) donors->insert(donors->end(), observed->begin(), observed->end());
            }
        }

        std::optional<std::vector<CollectiveReplacement>> replacements;
        const CollectiveGroup * selected = nullptr;
        std::string gap = work.error();
        for (const auto & donor : *donors) {
            if (!group.compatible(donor)) continue;
            auto donor_work = donor.work(invocation.input);
            if (!donor_work) continue;
            std::vector<CollectiveReplacement> candidate;
            for (std::size_t index = 0; index < group.members.size(); ++index) {
                auto replacement = CollectiveReplacement::plan(group.members[index], donor.members[index], (*donor_work)[index]);
                if (!replacement) {
                    gap = replacement.error();
                    break;
                }
                candidate.push_back(std::move(*replacement));
            }
            if (candidate.size() != group.members.size()) continue;
            replacements = std::move(candidate);
            selected = &donor;
            break;
        }
        if (!replacements) return std::unexpected(TemplateGap{ TemplateGapKind::Rule, gap, invocation.step, invocation.input.phase, 0 });

        // Instances still belong to the uncommitted cohort. Any later failure
        // discards all ranks before assembly can publish nodes or resource frontiers.
        std::map<BoundaryId, TemplateEndpoint> endpoints;
        for (std::size_t index = 0; index < group.members.size(); ++index) {
            const auto rank = group.members[index].definition->rank;
            auto & instance = instances[rank];
            auto & replacement = (*replacements)[index];
            const auto offsets = instance.topology.apply_operations(*instance.definition, std::span(&replacement.rewrite, 1));
            if (!offsets) return std::unexpected(TemplateGap{ TemplateGapKind::Rule, offsets.error(), invocation.step, invocation.input.phase, rank });
            for (auto & [from, to] : replacement.boundary_mapping) {
                if (to >= instance.definition->graph.boundary_count()) to += offsets->front();
                endpoints.emplace(selected->members[index].definition->boundary_observations[from].source_boundary,
                                  TemplateEndpoint{ instance.work_id, rank, to });
            }
            std::set<std::size_t> removed_boundaries;
            for (const auto connection : replacement.removed_connections) removed_boundaries.insert(instance.definition->connection(connection).local_boundary);
            std::erase_if(instance.connections, [&removed_boundaries](const TemplateTargetConnection & target) {
                return target.connection.relation == SourceRelation::NativeNotifySignal && removed_boundaries.contains(target.connection.local_boundary);
            });
        }
        for (std::size_t index = 0; index < group.members.size(); ++index) {
            auto & instance = instances[group.members[index].definition->rank];
            const auto & replacement = (*replacements)[index];
            const auto & donor = *selected->members[index].definition;
            for (const auto index : replacement.donor_connections) {
                auto connection = donor.connection(index);
                connection.local_boundary = replacement.boundary_mapping.at(connection.local_boundary);
                instance.connections.push_back({
                    std::move(connection),
                    { TemplateConnectionRule::NativeBoundary, { endpoints.at(donor.connection(index).remote_source_boundary) } }
                });
            }
        }
    }
    return {};
}

} // namespace markov
