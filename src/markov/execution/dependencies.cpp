#include "markov/execution/dependencies.hpp"

#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace markov {
namespace {

std::pair<std::size_t, std::size_t> interval(const CoarseGraph & graph, std::size_t scope, std::size_t resource) {
    const auto device = graph.resources.at(resource).kind == ResourceKind::Stream;
    const auto & owner = graph.scopes.at(scope);
    if (device && owner.communication) {

        // Communication owns its payload until actual completion, even if the caller stream has already returned.
        const auto & completion = *std::ranges::find_if(owner.ports, [](const auto & port) { return port.rendezvous.has_value(); });
        return { *completion.rendezvous, completion.event };
    }
    const auto start = owner.port(device ? "device.enter" : "call.enter", Direction::Input).event;
    const auto finish = owner.port(device ? "device.complete" : "call.return", Direction::Output).event;
    if (graph.events[start].resource != resource || graph.events[finish].resource != resource)
        throw std::invalid_argument("tensor lifetime has no boundary on its declared resource");
    return { start, finish };
}

} // namespace

GraphDependencies::GraphDependencies(KvPool & pool, ResourceService & resources, std::span<const Request> requests, ResourceObserver * observer)
    : pool_(pool), resources_(resources), observer_(observer) {
    for (std::size_t index = 0; index < requests.size(); ++index) requests_.emplace(requests[index].id, index);
}

void GraphDependencies::Contents::prune(const Timeline & timeline) {
    std::erase_if(active, [&](const auto & access) { return timeline.completed(access.finish); });
}

bool GraphDependencies::Contents::covers(std::int64_t begin, std::int64_t end) const {
    return std::ranges::any_of(initialized, [&](const auto & range) { return range.first <= begin && range.second >= end; });
}

void GraphDependencies::Contents::commit(const std::set<Access> & accesses, std::span<const std::size_t> events) {
    const auto previous_ranges = initialized.size();
    for (const auto & access : accesses) {
        active.push_back({ access.begin, access.end, access.write, events[access.start], events[access.finish], access.publication });
        if (access.write) initialized.emplace_back(access.begin, access.end);
    }
    if (initialized.size() == previous_ranges) return;

    // Merge this invocation's writes once; active completions still gate readers of pending contents.
    std::ranges::sort(initialized);
    std::vector<std::pair<std::int64_t, std::int64_t>> merged;
    for (const auto & range : initialized) {
        if (merged.empty() || merged.back().second < range.first) merged.push_back(range);
        else merged.back().second = std::max(merged.back().second, range.second);
    }
    initialized = std::move(merged);
}

void GraphDependencies::invalidate(const ResourceChanges & changes) {
    for (const auto page : changes.returned_pages) {

        // A logical return invalidates old contents now; in-flight hazards still protect physical reuse.
        for (auto it = pages_.lower_bound({ page, 0 }); it != pages_.end() && it->first.first == page; ++it) it->second.initialized.clear();
    }
    for (const auto slot : changes.returned_slots) {
        for (const auto & field : resources_.request_fields()) slots_[{ slot, field }].initialized.clear();
    }
    for (const auto & [field, slot] : changes.returned_fields) slots_[{ slot, field }].initialized.clear();
}

std::vector<std::size_t> GraphDependencies::insert(const CoarseGraph & graph, Timeline & timeline, std::span<const ServiceDuration> durations,
                                                   const std::map<std::string, GraphDependencies *> & pools, std::span<const BoundaryTime> boundaries) {
    std::set<std::string> exposed;
    for (const auto & port : graph.scopes.front().ports) {
        if (port.use == PortUse::Data && port.direction == Direction::Output) exposed.insert(port.tensor->storage);
    }

    // Aliased ports may expose one access repeatedly; distinct ranges, modes and event intervals stay distinct.
    std::map<Contents *, std::set<Access>> accesses;
    std::map<std::size_t, std::vector<std::pair<Contents *, Access>>> uninitialized;
    std::map<std::tuple<std::string, std::string, std::int64_t, std::int64_t>, std::vector<KvPageRange>> bindings;
    std::map<std::tuple<std::string, std::string, std::string>, std::size_t> state_bindings;
    std::set<std::tuple<std::size_t, std::size_t, Relation>> dependencies;

    // A successful read precedes subsequent work; it does not retroactively block the already returned query.
    const auto entered = graph.scopes.front().port("call.enter", Direction::Input).event;
    for (const auto event : observations_) dependencies.emplace(event, entered, Relation::Observation);
    for (const auto & port : graph.scopes.front().ports) {
        if (port.use != PortUse::Signal) continue;
        std::optional<std::size_t> producer;
        for (const auto & [_, owner] : pools) {
            if ((producer = owner->signal(port.signal))) break;
        }
        if (port.direction == Direction::Input) {
            if (!producer) throw std::invalid_argument("completion signal has no live producer: " + port.signal);
            dependencies.emplace(*producer, port.event, Relation::Signal);
        }
        else if (producer) throw std::invalid_argument("completion signal generation was already published: " + port.signal);
    }
    for (std::size_t scope = 0; scope < graph.scopes.size(); ++scope) {
        const auto & node = graph.scopes[scope];
        for (const auto & port : node.ports) {
            if (port.use != PortUse::Data) continue;
            const auto & tensor = *port.tensor;

            // Empty views carry a shape and storage identity, but read or overwrite no contents.
            if (std::ranges::find(tensor.shape, 0) != tensor.shape.end()) continue;
            if (tensor.allocation && !exposed.contains(tensor.storage)) continue;
            auto & contents = tensors_[tensor.storage];
            const auto resource = graph.events[port.event].resource;
            if (port.direction == Direction::Input) {

                // The declared input boundary locates consumption, including a host read completed by return.
                const auto finish = interval(graph, scope, resource).second;
                accesses[&contents].insert({ 0, 1, false, port.event, finish });
                if (!tensor.writer) {
                    const auto producer = values_.find(tensor.storage);
                    if (producer == values_.end() || producer->second.first != tensor.version)
                        throw std::invalid_argument("no producer for target input " + node.info.site + ":" + port.name);
                    dependencies.emplace(producer->second.second, port.event, Relation::Data);
                }
            }
            if (tensor.writer) {
                const auto [start, finish] = interval(graph, *tensor.writer, resource);
                accesses[&contents].insert({ 0, 1, true, start, finish });
            }
        }
        for (const auto & access : node.slot_accesses) {
            auto & owner = *pools.at(access.value.pool);
            for (const auto & request : access.value.requests) {
                const auto slot = owner.resources_.slot(access.value.field, owner.requests_.at(request));
                auto & contents = owner.slots_[{ slot, access.value.field }];
                const bool write = access.value.access == StateAccess::Write;
                Access region{ 0, 1, write, access.start, access.finish };
                if (!access.value.publication.empty()) region.publication = { owner.requests_.at(request), access.value.publication };
                accesses[&contents].insert(region);
                if (!write && !contents.covers(0, 1)) uninitialized[access.start].emplace_back(&contents, region);

                // Record the address actually used by execution once, without replaying field allocation for diagnostics.
                if (observer_) state_bindings.try_emplace({ access.value.pool, access.value.field, request }, slot);
            }
        }
        for (const auto & access : node.kv_accesses) {

            // One-sided transfers update the destination owner's contents without inventing a receive invocation.
            auto & owner = *pools.at(access.value.pool);
            for (const auto & region : access.value.regions) {
                auto ranges = owner.pool_.ranges(owner.requests_.at(region.request), region.begin, region.end);
                for (const auto & range : ranges) {
                    auto & contents = owner.pages_[{ range.page, access.value.layer }];
                    const bool write = access.value.access == StateAccess::Write;
                    for (const auto & [begin, end] : access.value.slice.ranges(range.begin, range.end)) {
                        const Access interval{ begin, end, write, access.start, access.finish };
                        accesses[&contents].insert(interval);
                        if (!write && !contents.covers(begin, end)) uninitialized[access.start].emplace_back(&contents, interval);
                    }
                }

                // Layers share the same physical mapping; diagnostics keep one copy per invocation and logical range.
                if (observer_) bindings.try_emplace({ access.value.pool, region.request, region.begin, region.end }, std::move(ranges));
            }
        }
    }

    if (!uninitialized.empty()) {
        std::vector<std::vector<std::size_t>> predecessors(graph.events.size());
        for (const auto & edge : graph.edges) predecessors[edge.target].push_back(edge.source);
        for (const auto & work : graph.work) predecessors[work.finish].push_back(work.start);

        for (const auto & [start, reads] : uninitialized) {

            // Only causally completed local writes can initialize a read; source numbering implies no order.
            std::vector<bool> preceding(graph.events.size());
            std::vector<std::size_t> pending{ start };
            preceding[start] = true;
            while (!pending.empty()) {
                const auto event = pending.back();
                pending.pop_back();
                for (const auto previous : predecessors[event]) {
                    if (preceding[previous]) continue;
                    preceding[previous] = true;
                    pending.push_back(previous);
                }
            }
            for (const auto & [contents, read] : reads) {
                auto ranges = contents->initialized;
                for (const auto & access : accesses.at(contents)) {
                    if (access.write && preceding[access.finish]) ranges.emplace_back(access.begin, access.end);
                }
                std::ranges::sort(ranges);
                auto covered = read.begin;
                for (const auto & [begin, end] : ranges) {
                    if (begin > covered) break;
                    covered = std::max(covered, end);
                }
                if (covered < read.end)
                    throw std::invalid_argument("state read at boundary " + std::to_string(start) + " has no causally preceding write for its full range");
            }
        }
    }

    // Only earlier invocations contribute hazards here; each action owns its internal causal graph.
    for (auto & [contents, current] : accesses) {
        contents->prune(timeline);
        for (const auto & access : current) {
            for (const auto & previous : contents->active) {

                // Equal publications carry the same value; readers and physical reuse still join every pending write.
                if (access.write && previous.write && access.publication && access.publication == previous.publication) continue;
                if ((access.write || previous.write) && access.begin < previous.end && previous.begin < access.end)
                    dependencies.emplace(previous.finish, access.start, Relation::State);
            }
        }
    }
    std::vector<ExecutionInput> inputs;
    for (const auto & [source, target, relation] : dependencies) inputs.push_back({ source, target, relation });
    const auto events = timeline.insert(graph, durations, inputs, boundaries);
    observations_.clear();
    for (const auto & [key, ranges] : bindings) {
        const auto & [pool, request, begin, end] = key;
        const auto & owner = *pools.at(pool);
        observer_->bound(events.at(entered),
                         pool,
                         request,
                         owner.pool_.residence(owner.requests_.at(request)).slot,
                         begin,
                         ranges);
    }
    for (const auto & [key, slot] : state_bindings) {
        const auto & [pool, field, request] = key;
        observer_->state_bound(events.at(entered), pool, field, request, slot);
    }
    for (auto & [contents, current] : accesses) contents->commit(current, events);
    for (const auto & port : graph.scopes.front().ports) {
        if (port.use == PortUse::Signal && port.direction == Direction::Output) signals_.emplace(port.signal, events[port.event]);
        if (port.use != PortUse::Data || port.direction != Direction::Output || !port.tensor->writer) continue;
        values_.insert_or_assign(port.tensor->storage, std::pair{ port.tensor->version, events[port.event] });
    }
    return events;
}

std::optional<std::size_t> GraphDependencies::signal(const std::string & name) const {
    const auto found = signals_.find(name);
    return found == signals_.end() ? std::nullopt : std::optional(found->second);
}

void GraphDependencies::observe_completion(std::size_t event) { observations_.insert(event); }

std::set<std::size_t> GraphDependencies::retain(const std::set<std::string> & bindings, const Timeline & timeline) {
    std::erase_if(values_, [&](const auto & value) { return !bindings.contains(value.first); });
    std::erase_if(signals_, [&](const auto & signal) { return !bindings.contains(signal.first); });
    for (auto it = tensors_.begin(); it != tensors_.end();) {
        it->second.prune(timeline);
        if (!bindings.contains(it->first) && it->second.active.empty()) it = tensors_.erase(it);
        else ++it;
    }
    for (auto & [_, contents] : pages_) contents.prune(timeline);
    for (auto & [_, contents] : slots_) contents.prune(timeline);

    // Active accesses finish in the future and the timeline keeps them; published values may already be complete.
    std::set<std::size_t> retained = observations_;
    for (const auto & [_, value] : values_) retained.insert(value.second);
    for (const auto & [_, event] : signals_) retained.insert(event);
    return retained;
}

} // namespace markov
