#include "markov/production/entries.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "markov/graph/state.hpp"

namespace markov {

class EntryExtraction::Cut {
public:
    Cut(const EntryExtraction & extraction, std::size_t root) : extraction_(extraction), source_(extraction.source_) {
        const auto & source = source_;
        graph_.environment = source.environment;
        graph_.resources = source.resources;
        std::vector<std::size_t> subtree{ root };
        for (std::size_t position = 0; position < subtree.size(); ++position) {
            const auto & children = extraction.scopes_.at(subtree[position]).children;
            subtree.insert(subtree.end(), children.begin(), children.end());
        }

        // Preserve source binding order, visiting only this call's descendants.
        std::ranges::sort(subtree);
        for (const auto scope : subtree) {
            auto copy = source.scopes[scope];
            copy.info.parent = scope == root ? std::nullopt : std::optional(scope_ids_.at(*copy.info.parent));
            if (scope == root) copy.info.arguments.clear();
            copy.ports.clear();
            copy.kv_accesses.clear();
            copy.slot_accesses.clear();
            scope_ids_.emplace(scope, graph_.scopes.size());
            graph_.scopes.push_back(std::move(copy));
        }

        // A cut retains channel order inside this action, not ordinals from preceding source calls.
        std::map<std::string, std::map<std::size_t, CommunicationGroup *>> communications;
        for (auto & scope : graph_.scopes) {
            if (!scope.communication) continue;
            auto & group = *scope.communication;
            if (!communications[group.channel].emplace(group.order, &group).second)
                throw std::invalid_argument("action repeats a communication channel position");
        }
        for (const auto & [channel, ordered] : communications) {
            std::size_t ordinal = 0;
            for (const auto & [source_order, group] : ordered) group->order = ordinal++;
        }

        for (const auto & [scope, _] : scope_ids_) {
            const auto & contents = extraction.scopes_[scope];
            for (const auto event : contents.events) event_ids_.try_emplace(event, 0);
            work_ids_.insert(work_ids_.end(), contents.work.begin(), contents.work.end());
            for (const auto & access : source.scopes[scope].kv_accesses) retain(access.start, access.finish);
            for (const auto & access : source.scopes[scope].slot_accesses) {
                retain(access.start, access.finish);
                if (access.value.access == StateAccess::Read) {
                    for (const auto & request : access.value.requests)
                        slot_inputs_[access.start].emplace(access.value.pool, access.value.field, request);
                }
            }
            for (const auto & port : source.scopes[scope].ports) {
                event_ids_.try_emplace(port.event, 0);
                if (port.direction == Direction::Input) {

                    // Only retained ports can replace an incoming edge; a discarded caller's declaration cannot bind this cut.
                    if (port.use == PortUse::Data) inputs_[port.event].emplace(port.tensor->storage, port.tensor->version);
                    if (port.use == PortUse::Signal) signal_inputs_[port.event].insert(port.signal);
                }
                if (port.rendezvous) event_ids_.try_emplace(*port.rendezvous, 0);
                if (port.wait) {
                    event_ids_.try_emplace(port.wait->begin, 0);
                    waits_.emplace(port.event, port.wait->resource);
                }
            }
        }
        std::ranges::sort(work_ids_);
        for (const auto identity : work_ids_) retain(source.work[identity].start, source.work[identity].finish);
    }

    CoarseGraph extract() {
        std::vector<std::vector<std::size_t>> resources(source_.resources.size());
        std::vector<std::size_t> ordered_events;
        for (const auto & [event, _] : event_ids_) ordered_events.push_back(event);
        std::ranges::sort(ordered_events, {}, [&](const auto event) { return std::pair{ source_.events[event].resource, extraction_.event_order_[event] }; });
        for (const auto event : ordered_events) {
            const auto & observed = source_.events[event];
            const auto owner = scope_ids_.contains(observed.scope) ? scope_ids_.at(observed.scope) : 0;
            event_ids_.at(event) = graph_.events.size();
            resources[observed.resource].push_back(graph_.events.size());
            graph_.events.push_back({ owner, observed.resource });
        }
        for (const auto & [scope, mapped] : scope_ids_) {
            for (const auto & access : source_.scopes[scope].kv_accesses)
                graph_.scopes[mapped].kv_accesses.push_back({ access.value, event_ids_.at(access.start), event_ids_.at(access.finish) });
            for (const auto & access : source_.scopes[scope].slot_accesses)
                graph_.scopes[mapped].slot_accesses.push_back({ access.value, event_ids_.at(access.start), event_ids_.at(access.finish) });
            for (const auto & port : source_.scopes[scope].ports) {
                auto copy = port;
                copy.event = event_ids_.at(port.event);
                if (copy.wait) copy.wait->begin = event_ids_.at(copy.wait->begin);
                if (copy.rendezvous) copy.rendezvous = event_ids_.at(*copy.rendezvous);
                if (copy.tensor && copy.tensor->writer) {
                    const auto found = scope_ids_.find(*copy.tensor->writer);
                    copy.tensor->writer = found == scope_ids_.end() ? std::nullopt : std::optional(found->second);
                }
                if (copy.tensor && copy.tensor->allocation) {
                    const auto found = scope_ids_.find(*copy.tensor->allocation);
                    copy.tensor->allocation = found == scope_ids_.end() ? std::nullopt : std::optional(found->second);
                }
                graph_.scopes[mapped].ports.push_back(std::move(copy));
            }
        }
        for (const auto identity : work_ids_) {
            const auto & piece = source_.work[identity];
            graph_.work.push_back({ scope_ids_.at(piece.scope), piece.resource, piece.kind, event_ids_.at(piece.start), event_ids_.at(piece.finish) });
        }
        for (const auto & [target, _] : event_ids_) {
            for (const auto identity : extraction_.incoming_[target]) connect(source_.edges[identity]);
        }

        std::size_t slot = 0;
        for (std::size_t resource = 0; resource < resources.size(); ++resource) {
            const auto & events = resources[resource];
            if (events.empty() || !source_.resources[resource].serial) continue;

            // Serial order is intrinsic; the preceding action is supplied by the target resource owner.
            for (std::size_t position = 1; position < events.size(); ++position)
                graph_.edges.push_back({ events[position - 1], events[position], Relation::Order });
            graph_.scopes[0].ports.push_back(
                { "resource.enter/" + std::to_string(slot), Direction::Input, PortUse::Control, Relation::Order, events.front(), {} });
            graph_.scopes[0].ports.push_back(
                { "resource.exit/" + std::to_string(slot), Direction::Output, PortUse::Control, Relation::Order, events.back(), {} });
            ++slot;
        }
        std::ranges::sort(graph_.edges);
        graph_.edges.erase(std::unique(graph_.edges.begin(), graph_.edges.end()), graph_.edges.end());
        graph_.validate();
        return std::move(graph_);
    }

private:
    void retain(std::size_t start, std::size_t finish) {
        event_ids_.try_emplace(start, 0);
        event_ids_.try_emplace(finish, 0);
    }

    void connect(const GraphEdge & edge) {

        // The cut rebuilds serial order once; retaining source copies only duplicates that same resource chain.
        const auto resource = source_.events[edge.target].resource;
        if (edge.kind == Relation::Order && source_.events[edge.source].resource == resource && source_.resources[resource].serial) return;

        // A wait resolves the target's submitted stream frontier, never the source run's last task.
        if (edge.kind == Relation::Signal && waits_.contains({ edge.target, source_.events[edge.source].resource })) return;
        if (event_ids_.contains(edge.source)) {
            graph_.edges.push_back({ event_ids_.at(edge.source), event_ids_.at(edge.target), edge.kind });
            return;
        }

        // A completed fact was read in the source; target control establishes its own observations, not a wait input.
        if (edge.kind == Relation::Observation) return;
        if (edge.kind == Relation::Data && declared_data(edge)) return;
        if (edge.kind == Relation::Signal && declared_signal(edge)) return;
        if (edge.kind == Relation::State && declared_state(edge)) return;
        throw std::invalid_argument("action " + graph_.scopes[0].info.site + " lacks a declared input for " + std::string(relation_name(edge.kind))
                                    + " dependency " + std::to_string(edge.source) + " -> " + std::to_string(edge.target));
    }

    bool declared_data(const GraphEdge & edge) const {
        const auto producer = extraction_.outputs_.find(edge.source);
        const auto consumer = inputs_.find(edge.target);
        if (producer == extraction_.outputs_.end() || consumer == inputs_.end()) return false;
        return std::ranges::any_of(consumer->second, [&](const auto & tensor) { return producer->second.contains(tensor); });
    }

    bool declared_signal(const GraphEdge & edge) const {
        const auto producer = extraction_.signal_outputs_.find(edge.source);
        const auto consumer = signal_inputs_.find(edge.target);
        if (producer == extraction_.signal_outputs_.end() || consumer == signal_inputs_.end()) return false;
        return std::ranges::any_of(consumer->second, [&](const auto & signal) { return producer->second.contains(signal); });
    }

    bool declared_state(const GraphEdge & edge) const {
        const auto producer = extraction_.slot_outputs_.find(edge.source);
        const auto consumer = slot_inputs_.find(edge.target);
        if (producer == extraction_.slot_outputs_.end() || consumer == slot_inputs_.end()) return false;

        // The target resource owner restores field dataflow; a cut keeps the exact pool/field/request input.
        return std::ranges::any_of(consumer->second, [&](const auto & value) { return producer->second.contains(value); });
    }

    const EntryExtraction & extraction_;
    const CoarseGraph & source_;
    CoarseGraph graph_;
    std::vector<std::size_t> work_ids_;
    std::map<std::size_t, std::size_t> scope_ids_;
    std::map<std::size_t, std::size_t> event_ids_;
    std::set<std::pair<std::size_t, std::size_t>> waits_;
    std::map<std::size_t, std::set<std::pair<std::string, std::int64_t>>> inputs_;
    std::map<std::size_t, std::set<std::string>> signal_inputs_;
    std::map<std::size_t, std::set<SlotContent>> slot_inputs_;
};

EntryExtraction::EntryExtraction(const CoarseGraph & source)
    : source_(source),
      scopes_(source.scopes.size()),
      incoming_(source.events.size()),
      event_order_(source.events.size()) {

    // Every cut shares one immutable source order and boundary index; only local bindings are rebuilt.
    std::vector<bool> retained(source.events.size());
    for (const auto & scope : source.scopes) {
        for (const auto & port : scope.ports) {
            retained[port.event] = true;
            if (port.wait) retained[port.wait->begin] = true;
            if (port.rendezvous) retained[*port.rendezvous] = true;
            if (port.direction != Direction::Output) continue;
            if (port.use == PortUse::Signal) signal_outputs_[port.event].insert(port.signal);
            if (port.use == PortUse::Data) outputs_[port.event].emplace(port.tensor->storage, port.tensor->version);
        }
        for (const auto & access : scope.slot_accesses) {
            retained[access.start] = retained[access.finish] = true;
            if (access.value.access != StateAccess::Write) continue;
            for (const auto & request : access.value.requests)
                slot_outputs_[access.finish].emplace(access.value.pool, access.value.field, request);
        }
        for (const auto & access : scope.kv_accesses) retained[access.start] = retained[access.finish] = true;
    }

    for (std::size_t scope = 0; scope < source.scopes.size(); ++scope) {
        if (const auto parent = source.scopes[scope].info.parent) scopes_[*parent].children.push_back(scope);
    }
    for (std::size_t work = 0; work < source.work.size(); ++work) {
        const auto & piece = source.work[work];
        scopes_[piece.scope].work.push_back(work);
        retained[piece.start] = retained[piece.finish] = true;
    }
    for (std::size_t identity = 0; identity < source.edges.size(); ++identity) {
        const auto & edge = source.edges[identity];
        incoming_[edge.target].push_back(identity);
        const auto resource = source.events[edge.source].resource;
        if (edge.kind != Relation::Order || source.events[edge.target].resource != resource || !source.resources[resource].serial)
            retained[edge.source] = retained[edge.target] = true;
    }

    // Unnamed empty serial markers add no work or dependency; each cut reconstructs order between retained boundaries.
    for (std::size_t event = 0; event < source.events.size(); ++event) {
        const auto & boundary = source.events[event];
        if (retained[event] || !source.resources[boundary.resource].serial) scopes_[boundary.scope].events.push_back(event);
    }

    std::vector<std::set<std::size_t>> next(source_.events.size());
    for (const auto & work : source_.work) next[work.start].insert(work.finish);
    for (const auto & edge : source_.edges) next[edge.source].insert(edge.target);
    std::vector<std::size_t> remaining(next.size());
    for (const auto & targets : next) {
        for (const auto target : targets) ++remaining[target];
    }
    std::deque<std::size_t> ready;
    for (std::size_t event = 0; event < remaining.size(); ++event) {
        if (remaining[event] == 0) ready.push_back(event);
    }
    std::size_t position = 0;
    while (!ready.empty()) {
        const auto event = ready.front();
        ready.pop_front();
        event_order_[event] = position++;
        for (const auto target : next[event]) {
            if (--remaining[target] == 0) ready.push_back(target);
        }
    }
}

CoarseGraph EntryExtraction::extract(std::size_t scope) const { return Cut(*this, scope).extract(); }

} // namespace markov
