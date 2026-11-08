#include "markov/adapters/sglang/templates.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "markov/production/templates.hpp"

namespace markov::sglang {

namespace {

class ActionConditions {
public:
    explicit ActionConditions(const CoarseGraph & graph)
        : graph_(graph),
          children_(graph.scopes.size()),
          host_work_(graph.scopes.size()),
          other_work_(graph.scopes.size()) {
        for (std::size_t scope = 0; scope < graph.scopes.size(); ++scope) {
            if (const auto parent = graph.scopes[scope].info.parent) children_[*parent].push_back(scope);
        }
        for (const auto & work : graph.work) {
            const auto & resource = graph.resources[work.resource];
            if (work.kind == WorkKind::Host && resource.kind == ResourceKind::Thread && resource.role == "control") ++host_work_[work.scope];
            else other_work_[work.scope] = true;
        }
    }

    nlohmann::json segment(std::size_t scope) const {
        const auto & info = graph_.scopes[scope].info;
        const auto & segment = info.bindings.at("segment");
        nlohmann::json condition{
            { "owner", segment.at("owner") }
        };
        if (segment.value("host_only", false)) {
            validate_host(scope);
            return condition;
        }

        const auto & siblings = children_.at(info.parent.value());
        const auto position = std::ranges::find(siblings, scope);

        // Observed child calls identify the continuation; poll counts and source ordinals do not.
        condition["entry"] = position == siblings.begin() ? "call.enter" : operation(*std::prev(position));
        condition["exit"] = std::next(position) == siblings.end() ? "call.return" : operation(*std::next(position));
        return condition;
    }

    std::set<std::string> bind_state(std::size_t scope, ScopeInfo & info) const {
        std::set<std::string> accesses;
        std::vector<std::size_t> pending{ scope };

        // An action owns only its subtree; unrelated later actions cannot contribute request-state bindings.
        for (std::size_t position = 0; position < pending.size(); ++position) {
            const auto child = pending[position];
            for (const auto nested : children_[child]) pending.push_back(nested);
            for (const auto & access : graph_.scopes[child].slot_accesses) {
                const auto & value = access.value;
                accesses.insert(value.field + "/" + std::string(state_access_name(value.access)));

                // Background work must use the action's member binding, not a captured source request ID.
                if (info.work.contains(value.members) && info.work.at(value.members) != value.requests)
                    throw std::invalid_argument("action children disagree on request-state member binding: " + value.members);
                info.work[value.members] = value.requests;
            }
        }
        return accesses;
    }

    void validate(std::size_t scope, std::string_view action, const nlohmann::json & condition) const {
        const bool control_transform = action == "batch_transform" && condition.at("transformation").at("effect") == "control";
        const bool control_segment = action == "segment" && condition.value("effect", std::string{}) == "control";
        if (control_transform || control_segment) validate_host(scope);
    }

private:
    void validate_host(std::size_t scope) const {
        const auto & observed = graph_.scopes[scope];
        const bool host_ports = observed.ports.size() == 2 && std::ranges::all_of(observed.ports, [&](const auto & port) {
                                    const auto & resource = graph_.resources[graph_.events[port.event].resource];
                                    const bool host = (port.name == "call.enter" || port.name == "call.return") && resource.kind == ResourceKind::Thread
                                                      && resource.role == "control";
                                    return host && port.use == PortUse::Control && port.relation == Relation::Control && !port.tensor && !port.wait
                                           && !port.rendezvous && port.signal.empty();
                                });

        // A declared control continuation may share its CPU structure, never hidden device or state effects.
        if (host_work_[scope] != 1 || other_work_[scope] || !children_[scope].empty() || !host_ports || observed.communication || !observed.kv_accesses.empty()
            || !observed.slot_accesses.empty())
            throw std::invalid_argument("host-only control fragment contains an undeclared execution boundary: " + observed.info.site);
    }

    std::string operation(std::size_t scope) const {
        const auto & info = graph_.scopes[scope].info;
        if (info.role != "invocation") return info.role;
        const auto & first = graph_.scopes[children_.at(scope).at(0)].info;
        return first.bindings.at("segment").at("owner").get<std::string>();
    }

    const CoarseGraph & graph_;
    std::vector<std::vector<std::size_t>> children_;
    std::vector<std::size_t> host_work_;
    std::vector<bool> other_work_;
};

} // namespace

TemplateLibrary NativeTemplates::compile(const CoarseGraph & observation) {
    auto graph = observation;
    const ActionConditions conditions(observation);
    std::vector<EntrySpecification> entries;
    for (std::size_t scope = 0; scope < graph.scopes.size(); ++scope) {
        auto & info = graph.scopes[scope].info;
        const auto & observed = observation.scopes[scope].info;

        // Entry selection is separate from per-worker model and collective bindings.
        info.bindings = nlohmann::json::object();
        for (const auto name : { "weights", "collective_group", "local_implementation" }) {
            if (observed.bindings.contains(name)) info.bindings[name] = observed.bindings.at(name);
        }

        // Native selection can run between PD caller segments while remaining an independently submitted action.
        const bool action = info.role == "segment" || observed.bindings.value("action", false) || (info.parent == 0 && info.role != "invocation");
        if (scope == 0 || !action) continue;

        nlohmann::json condition{
            { "phase", info.work.value("phase", std::string("idle")) }
        };
        condition["state_accesses"] = conditions.bind_state(scope, info);
        if (observed.bindings.contains("condition")) condition.update(observed.bindings.at("condition"));
        if (info.role == "segment") condition.update(conditions.segment(scope));
        if (info.role == "signal_wait") condition["device_wait"] = observed.bindings.at("device_wait");
        if (info.role == "stream_wait") condition["streams"] = observed.bindings.at("streams");
        if (info.role == "gather") condition["collective"] = observed.bindings.at("collective");
        if (info.role == "transfer_send" || info.role == "transfer_receive") condition["dtype"] = observed.bindings.at("dtype");
        condition = NativeTemplates::condition(info.role, std::move(condition));
        conditions.validate(scope, info.role, condition);
        entries.push_back({ scope, info.role, std::move(condition) });
    }
    auto library = TemplateCompiler::compile(graph, entries, domain(graph.environment));

    // Tensor collective code depends on this runtime; geometry, dtype, layout and group roles belong to the local call contract.
    library.local_domain = nlohmann::json::object();
    for (const auto field : { "framework", "device", "torch", "torch_npu" }) library.local_domain[field] = graph.environment.at(field);
    library.collect_local_implementations();
    library.validate();
    return library;
}

nlohmann::json NativeTemplates::condition(std::string_view action, nlohmann::json facts) {
    if (action == "segment" && facts.at("owner") == "result") {

        // Token-only results consume device output before request/cache processing; child calls own resource effects.
        facts["effect"] = facts.at("entry") == "call.enter" ? "result_input" : "control";
        facts.erase("entry");
        facts.erase("exit");
        return facts;
    }
    if (action != "batch_transform") return facts;
    auto & transformation = facts.at("transformation");
    const auto outcome = transformation.at("outcome").get<std::string>();

    // Native filter returns before tensor operations for both empty and unchanged membership.
    const bool control_only = transformation.at("operation") == "filter" && (outcome == "empty" || outcome == "unchanged");
    transformation["effect"] = control_only ? "control" : outcome;
    transformation.erase("outcome");
    return facts;
}

nlohmann::json NativeTemplates::domain(const nlohmann::json & environment) {
    if (environment.at("framework") != "sglang") throw std::invalid_argument("target framework has no SGLang implementation domain");
    auto result = environment;

    // Placement and initial state are target bindings; implementation and worker layout still require matching evidence.
    for (const auto field : { "device_index", "initial_state", "closure" }) result.erase(field);
    result.at("parallel").erase("rank");
    return result;
}

} // namespace markov::sglang
