#include "markov/graph/graph.hpp"

#include <algorithm>
#include <compare>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace markov {
namespace {

std::size_t identity(const nlohmann::json & value) {
    if (!value.is_number_integer() || value.get<std::int64_t>() < 0) throw std::invalid_argument("graph identity must be a nonnegative integer");
    return value.get<std::size_t>();
}

} // namespace

CoarseGraph CoarseGraph::read(const nlohmann::json & source) {
    CoarseGraph result;
    result.environment = source.at("environment");
    for (const auto & resource : source.at("resources").get_ref<const nlohmann::json::array_t &>()) result.resources.push_back(Resource::read(resource));

    for (const auto & scope : source.at("scopes").get_ref<const nlohmann::json::array_t &>()) {
        GraphScope observed{ ScopeInfo::read(scope), {} };
        if (scope.contains("communication")) observed.communication = CommunicationGroup::read(scope.at("communication"));
        for (const auto & port : scope.at("ports").get_ref<const nlohmann::json::array_t &>()) {
            GraphPort boundary{ port.at("name").get<std::string>(),
                                direction_kind(port.at("direction").get<std::string>()),
                                port_use(port.at("use").get<std::string>()),
                                relation_kind(port.at("relation").get<std::string>()),
                                identity(port.at("event")),
                                std::nullopt };
            if (port.contains("tensor") && !port.at("tensor").is_null()) boundary.tensor = TensorObservation::read(port.at("tensor"));
            if (port.contains("wait")) {
                const auto & wait = port.at("wait");
                boundary.wait = StreamWait<std::size_t>{ identity(wait.at("resource")), identity(wait.at("begin")) };
            }
            if (port.contains("rendezvous")) boundary.rendezvous = identity(port.at("rendezvous"));
            if (boundary.use == PortUse::Signal) boundary.signal = port.at("signal").get<std::string>();
            observed.ports.push_back(std::move(boundary));
        }
        for (const auto & access : scope.at("kv_accesses").get_ref<const nlohmann::json::array_t &>())
            observed.kv_accesses.push_back({ KvAccess::read(access), identity(access.at("start")), identity(access.at("finish")) });
        for (const auto & access : scope.at("slot_accesses"))
            observed.slot_accesses.push_back({ SlotAccess::read(access), identity(access.at("start")), identity(access.at("finish")) });
        result.scopes.push_back(std::move(observed));
    }

    for (const auto & event : source.at("events").get_ref<const nlohmann::json::array_t &>())
        result.events.push_back({ identity(event.at("scope")), identity(event.at("resource")) });
    for (const auto & piece : source.at("work").get_ref<const nlohmann::json::array_t &>())
        result.work.push_back({ identity(piece.at("scope")),
                                identity(piece.at("resource")),
                                work_kind(piece.at("kind").get<std::string>()),
                                identity(piece.at("start")),
                                identity(piece.at("finish")) });
    for (const auto & edge : source.at("edges").get_ref<const nlohmann::json::array_t &>())
        result.edges.push_back({ identity(edge.at("source")), identity(edge.at("target")), relation_kind(edge.at("kind").get<std::string>()) });

    result.validate();
    return result;
}

nlohmann::json CoarseGraph::document(bool include_environment) const {
    nlohmann::json result{
        { "resources", nlohmann::json::array() },
        {    "scopes", nlohmann::json::array() },
        {    "events", nlohmann::json::array() },
        {      "work", nlohmann::json::array() },
        {     "edges", nlohmann::json::array() }
    };
    if (include_environment) result["environment"] = environment;
    for (const auto & resource : resources) result["resources"].push_back(resource.document());
    for (const auto & scope : scopes) {
        auto record = scope.info.document();
        if (scope.communication) record["communication"] = scope.communication->document();
        record["ports"] = nlohmann::json::array();
        record["slot_accesses"] = nlohmann::json::array();
        for (const auto & access : scope.slot_accesses) {
            auto value = access.value.document();
            value["start"] = access.start;
            value["finish"] = access.finish;
            record["slot_accesses"].push_back(std::move(value));
        }
        record["kv_accesses"] = nlohmann::json::array();
        for (const auto & access : scope.kv_accesses) {
            auto value = access.value.document();
            value["start"] = access.start;
            value["finish"] = access.finish;
            record["kv_accesses"].push_back(std::move(value));
        }
        for (const auto & port : scope.ports) {
            nlohmann::json boundary{
                {      "name",                      port.name },
                { "direction", direction_name(port.direction) },
                {       "use",        port_use_name(port.use) },
                {  "relation",   relation_name(port.relation) },
                {     "event",                     port.event }
            };
            if (port.tensor) boundary["tensor"] = port.tensor->document();
            if (port.wait)
                boundary["wait"] = {
                    { "resource", port.wait->resource },
                    {    "begin",    port.wait->begin }
                };
            if (port.rendezvous) boundary["rendezvous"] = *port.rendezvous;
            if (port.use == PortUse::Signal) boundary["signal"] = port.signal;
            record["ports"].push_back(std::move(boundary));
        }
        result["scopes"].push_back(std::move(record));
    }
    for (const auto & event : events)
        result["events"].push_back({
            {    "scope",    event.scope },
            { "resource", event.resource }
        });
    for (const auto & piece : work)
        result["work"].push_back({
            {    "scope",           piece.scope },
            { "resource",        piece.resource },
            {     "kind", work_name(piece.kind) },
            {    "start",           piece.start },
            {   "finish",          piece.finish }
        });
    for (const auto & edge : edges)
        result["edges"].push_back({
            { "source",              edge.source },
            { "target",              edge.target },
            {   "kind", relation_name(edge.kind) }
        });
    return result;
}

bool CoarseGraph::contains(std::size_t scope, std::size_t descendant) const {
    while (descendant > scope) descendant = *scopes[descendant].info.parent;
    return descendant == scope;
}

std::size_t CoarseGraph::common_scope(std::size_t left, std::size_t right) const {
    while (left != right) {
        if (left > right) left = *scopes[left].info.parent;
        else right = *scopes[right].info.parent;
    }
    return left;
}

const GraphPort & GraphScope::port(std::string_view name, Direction direction) const {
    const auto found = std::ranges::find_if(ports, [&](const auto & boundary) { return boundary.name == name && boundary.direction == direction; });
    if (found == ports.end()) throw std::invalid_argument("scope has no required port: " + std::string(name));
    return *found;
}

void CoarseGraph::validate() const {
    if (!environment.is_object() || scopes.empty()) throw std::invalid_argument("coarse graph requires an environment and a root scope");
    std::set<std::tuple<ResourceKind, std::int64_t, std::string>> resource_ids;
    for (const auto & resource : resources) {
        if (!resource_ids.emplace(resource.kind, resource.rank, resource.identity).second)
            throw std::invalid_argument("coarse graph declares the same native resource twice");
    }
    std::set<std::pair<std::size_t, std::size_t>> ordered_intervals;
    std::map<std::string, std::string> buffers;
    for (std::size_t scope = 0; scope < scopes.size(); ++scope) {
        const auto parent = scopes[scope].info.parent;
        if ((scope == 0 && parent) || (scope != 0 && (!parent || *parent >= scope)))
            throw std::invalid_argument("graph scopes require one root and parents before children");

        if (scopes[scope].communication) {
            scopes[scope].communication->validate();
            for (const auto & field : scopes[scope].communication->agreements) {
                const auto & port = scopes[scope].port(field.port, field.direction);
                if ((port.use != PortUse::Data && port.use != PortUse::Metadata) || !port.tensor)
                    throw std::invalid_argument("communication payload agreement requires a tensor port");
            }
        }
        std::size_t rendezvous = 0;
        std::set<std::pair<Direction, std::string>> ports;
        for (const auto & port : scopes[scope].ports) {
            if (port.name.empty() || !ports.emplace(port.direction, port.name).second || port.event >= events.size())
                throw std::invalid_argument("scope has an invalid or duplicate port");
            if (port.use == PortUse::Data && !port.tensor) throw std::invalid_argument("data ports require a tensor binding");
            if (port.use == PortUse::Signal && (port.signal.empty() || port.relation != Relation::Signal || port.tensor || port.wait || port.rendezvous))
                throw std::invalid_argument("completion signals require an identity and a pure signal boundary");
            if (port.tensor && port.tensor->writer && *port.tensor->writer >= scopes.size())
                throw std::invalid_argument("tensor writer is outside the observed scope tree");
            if (port.tensor && port.tensor->allocation && *port.tensor->allocation >= scopes.size())
                throw std::invalid_argument("tensor allocation is outside the observed scope tree");
            if (port.tensor && !port.tensor->buffer.empty()) {
                const auto & tensor = *port.tensor;
                const auto [found, inserted] = buffers.emplace(tensor.buffer, tensor.storage);
                if (tensor.allocation || (!inserted && found->second != tensor.storage))
                    throw std::invalid_argument("declared persistent buffer changes its allocation");
            }
            if (port.rendezvous) {
                ++rendezvous;
                if (!scopes[scope].communication || *port.rendezvous >= events.size() || port.direction != Direction::Output || port.use != PortUse::Control
                    || port.relation != Relation::Communication || port.tensor || port.wait)
                    throw std::invalid_argument("invalid communication completion port");
                ordered_intervals.emplace(*port.rendezvous, port.event);
            }
            if (port.wait) {
                const auto & wait = *port.wait;
                if (port.direction != Direction::Input || port.use != PortUse::Control || port.relation != Relation::Signal || port.tensor
                    || wait.resource >= resources.size() || resources[wait.resource].kind != ResourceKind::Stream || wait.begin >= events.size()
                    || events[wait.begin].resource != events[port.event].resource)
                    throw std::invalid_argument("invalid stream wait port");
                if (resources.at(events[port.event].resource).kind != ResourceKind::Thread)
                    throw std::invalid_argument("stream synchronization requires a host thread boundary");
                if (wait.begin != port.event) ordered_intervals.emplace(wait.begin, port.event);
            }
        }
        if (rendezvous != (scopes[scope].communication ? 1U : 0U)) throw std::invalid_argument("communication scope requires one completion port");
    }
    for (const auto & event : events) {
        if (event.scope >= scopes.size() || event.resource >= resources.size()) throw std::invalid_argument("graph event has an undefined scope or resource");
    }
    for (std::size_t scope = 0; scope < scopes.size(); ++scope) {
        for (const auto & port : scopes[scope].ports) {
            if (port.rendezvous
                && (resources[events[*port.rendezvous].resource].rank != scopes[scope].communication->rank
                    || resources[events[port.event].resource].rank != scopes[scope].communication->rank))
                throw std::invalid_argument("communication boundary belongs to another rank");
        }
        const auto check_interval = [&](const auto & access) {
            if (access.value.pool.empty()) throw std::invalid_argument("state access has no pool binding");
            if (access.start >= events.size() || access.finish >= events.size() || !contains(events[access.start].scope, scope)
                || !contains(events[access.finish].scope, scope))
                throw std::invalid_argument("state access has an invalid semantic interval");
            const auto resource = events[access.start].resource;
            if (resource != events[access.finish].resource) throw std::invalid_argument("state access interval changes execution resource");
            ordered_intervals.emplace(access.start, access.finish);
        };
        for (const auto & access : scopes[scope].kv_accesses) {
            access.value.slice.validate();
            check_interval(access);
        }
        for (const auto & access : scopes[scope].slot_accesses) {
            if (access.value.field.empty() || access.value.members.empty()) throw std::invalid_argument("slot access has an incomplete binding");
            check_interval(access);
        }
    }

    // A work interval contributes start -> finish; waiting itself contributes no additional cost.
    std::vector<std::set<std::size_t>> successors(events.size());
    std::vector<std::size_t> remaining(events.size());
    std::vector<std::optional<std::size_t>> work_finish(events.size());
    for (const auto & piece : work) {
        if (piece.scope >= scopes.size() || piece.resource >= resources.size() || piece.start >= events.size() || piece.finish >= events.size()
            || piece.start == piece.finish || piece.kind == WorkKind::Wait || piece.kind == WorkKind::Boundary)
            throw std::invalid_argument("invalid coarse work interval");
        if (events[piece.start].resource != piece.resource || events[piece.finish].resource != piece.resource)
            throw std::invalid_argument("work interval changes resource between entry and completion");
        if (!contains(events[piece.start].scope, piece.scope) || !contains(events[piece.finish].scope, piece.scope))
            throw std::invalid_argument("work interval is owned by an unrelated semantic scope");
        if (work_finish[piece.start]) throw std::invalid_argument("coarse work is charged twice at one entry boundary");
        work_finish[piece.start] = piece.finish;
        successors[piece.start].insert(piece.finish);
    }
    for (const auto & edge : edges) {
        if (edge.source >= events.size() || edge.target >= events.size() || edge.source == edge.target)
            throw std::invalid_argument("invalid coarse dependency endpoint");
        successors[edge.source].insert(edge.target);
    }
    for (const auto & targets : successors) {
        for (const auto target : targets) ++remaining[target];
    }

    std::deque<std::size_t> ready;
    for (std::size_t event = 0; event < events.size(); ++event) {
        if (remaining[event] == 0) ready.push_back(event);
    }
    std::vector<std::optional<std::size_t>> frontiers(resources.size());
    std::vector<std::size_t> positions(events.size());
    std::size_t visited = 0;
    while (!ready.empty()) {
        const auto event = ready.front();
        ready.pop_front();
        positions[event] = visited++;

        // Serial resources must carry an explicit chain, not rely on a topological tie-break.
        const auto resource = events[event].resource;
        if (resources[resource].serial) {
            if (frontiers[resource] && !successors[*frontiers[resource]].contains(event))
                throw std::invalid_argument("serial resource has incomparable or discontinuous event boundaries");
            if (frontiers[resource] && work_finish[*frontiers[resource]] && *work_finish[*frontiers[resource]] != event)
                throw std::invalid_argument("coarse work crosses an unsplit serial resource boundary");
            frontiers[resource] = event;
        }
        for (const auto target : successors[event]) {
            if (--remaining[target] == 0) ready.push_back(target);
        }
    }
    if (visited != events.size()) throw std::invalid_argument("coarse graph contains a dependency cycle");

    // Topological order alone cannot prove causality across resources or concurrent service intervals.
    for (const auto & [start, finish] : ordered_intervals) {
        if (positions[start] >= positions[finish]) throw std::invalid_argument("semantic interval must start before it finishes");
        const auto resource = events[start].resource;
        if (resource == events[finish].resource && resources[resource].serial) continue;

        std::vector<bool> reached(events.size());
        std::vector<std::size_t> pending{ start };
        reached[start] = true;
        while (!pending.empty() && !reached[finish]) {
            const auto event = pending.back();
            pending.pop_back();
            for (const auto target : successors[event]) {
                if (reached[target] || positions[target] > positions[finish]) continue;
                reached[target] = true;
                pending.push_back(target);
            }
        }
        if (!reached[finish])
            throw std::invalid_argument("semantic interval has no causal path: " + std::to_string(start) + " -> " + std::to_string(finish));
    }
}

} // namespace markov
