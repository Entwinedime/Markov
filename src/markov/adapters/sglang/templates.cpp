#include "markov/adapters/sglang/templates.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "markov/production/templates.hpp"

namespace markov::sglang {

namespace {

void declare_payload_agreements(GraphScope & scope) {
    if (!scope.communication) return;
    auto & group = *scope.communication;
    constexpr std::array<std::string_view, 7> uniform{
        "all_reduce", "quant_all_reduce", "broadcast", "all_gather", "all_gather_into_tensor", "reduce_scatter", "reduce_scatter_tensor"
    };
    const bool fixed_shape = std::ranges::find(uniform, group.operation) != uniform.end();
    const bool expert = group.operation == "dispatch" || group.operation == "combine";
    if (!fixed_shape && !expert && group.operation != "all_to_all_single") return;

    // Agreement uses observed ports; absent native geometry is not reconstructed from the enclosing model shape.
    group.agreements.clear();
    for (const auto & port : scope.ports) {
        if (!port.tensor || (port.use != PortUse::Data && port.use != PortUse::Metadata)) continue;
        if (expert && port.name != "hidden_states") continue;
        group.agreements.push_back({ port.name, port.direction, fixed_shape });
    }
}

std::vector<std::size_t> batch_resources(const TemplateLibrary & library) {
    std::set<std::size_t> resources;
    for (const auto & entry : library.entries) {
        const auto owner = entry.condition.value("owner", std::string{});
        if (entry.action == "batch_transform" || (entry.action == "segment" && (owner == "preparation" || owner == "allocation")))
            resources.insert(entry.frame.resources.begin(), entry.frame.resources.end());
    }

    // Observed batch preparation identifies the foreground lane independently of background KV transport.
    return { resources.begin(), resources.end() };
}

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
        const auto & continuation = info.work.at("continuation");
        if (continuation.at("owner") != segment.at("owner"))
            throw std::invalid_argument("segment continuation names a different native owner: " + info.site);
        const auto returned = graph_.scopes[scope].port("call.return", Direction::Output).event;
        if (std::ranges::any_of(graph_.scopes[scope].ports, [&](const auto & port) {
                return port.direction == Direction::Input && port.use == PortUse::Data && port.event == returned;
            }))
            throw std::invalid_argument("completed host reads require their own materialization action: " + info.site);
        nlohmann::json condition{
            { "owner", segment.at("owner") }
        };
        if (segment.value("host_only", false)) {
            validate_host(scope);
            return condition;
        }

        // Capture and control already share these semantic cuts; observer scopes cannot redefine them.
        for (const auto name : { "entry", "exit" }) {
            const auto & boundary = continuation.at(name).get_ref<const std::string &>();
            if (boundary.empty()) throw std::invalid_argument("segment has an unfinished native continuation: " + info.site);
            condition[name] = boundary;
        }
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
        const bool result_continuation = action == "segment" && condition.at("owner") == "result";
        if (control_transform || result_continuation) validate_host(scope);
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

    const CoarseGraph & graph_;
    std::vector<std::vector<std::size_t>> children_;
    std::vector<std::size_t> host_work_;
    std::vector<bool> other_work_;
};

} // namespace

TemplateDeclarations NativeTemplates::describe(const CoarseGraph & observation) {
    auto graph = observation;
    const ActionConditions conditions(observation);
    std::vector<EntrySpecification> entries;
    for (std::size_t scope = 0; scope < graph.scopes.size(); ++scope) {
        declare_payload_agreements(graph.scopes[scope]);
        auto & info = graph.scopes[scope].info;
        const auto & observed = observation.scopes[scope].info;

        // Entry selection is separate from per-worker model and collective bindings.
        info.bindings = nlohmann::json::object();
        for (const auto name : { "weights", "collective_group", "local_implementation" }) {
            if (observed.bindings.contains(name)) info.bindings[name] = observed.bindings.at(name);
        }

        // Expert tensor calls retain their initialized model domain, independently of the enclosing serving phase.
        const auto & communication = observation.scopes[scope].communication;
        if (info.role == "expert_compute" || (communication && (communication->operation == "dispatch" || communication->operation == "combine"))) {
            const auto & owner = observation.scopes.at(*observed.parent).info;
            info.bindings["local_implementation"] = owner.bindings.at("local_implementation");
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
        if (info.role == "signal_wait") {
            const bool device = observed.bindings.at("device_wait").get<bool>();
            condition["device_wait"] = device;
            if (device) condition["stream"] = observed.work.at("context").at("wait_stream").get<std::string>();
        }
        if (info.role == "signal_record") condition["stream"] = observed.work.at("context").at("record_stream").get<std::string>();
        if (info.role == "stream_wait") condition["streams"] = observed.bindings.at("streams");
        if (info.role == "gather") condition["collective"] = observed.bindings.at("collective");
        if (info.role == "transfer_send" || info.role == "transfer_receive") condition["dtype"] = observed.bindings.at("dtype");
        condition = NativeTemplates::condition(info.role, std::move(condition));
        conditions.validate(scope, info.role, condition);
        entries.push_back({ scope, info.role, std::move(condition) });
    }
    return { std::move(graph), std::move(entries) };
}

TemplateLibrary NativeTemplates::compile(const CoarseGraph & observation) {
    const auto declarations = describe(observation);
    const auto & graph = declarations.graph;
    auto library = TemplateCompiler::compile(graph, declarations.entries, domain(graph.environment));

    // All candidate libraries belong to one initialized model/layout; PD mode and capacity remain caller-owned.
    library.local_domain = nlohmann::json::object();
    for (const auto field : { "framework", "device", "torch", "torch_npu" }) library.local_domain[field] = graph.environment.at(field);
    const auto & implementation = graph.environment.at("implementation");
    for (const auto field : { "model", "dtype", "load_format", "framework_revision" }) library.local_domain[field] = implementation.at(field);
    library.local_domain["expert_layout"] = implementation.value("expert_layout", nlohmann::json());
    library.local_domain["expert_layers"] = graph.environment.value("expert_layers", nlohmann::json::array());
    for (const auto field : { "logical_tp", "logical_pp", "logical_ep" }) library.local_domain[field] = graph.environment.at("parallel").at(field);
    library.collect_local_implementations();
    library.validate();
    return library;
}

nlohmann::json NativeTemplates::condition(std::string_view action, nlohmann::json facts) {
    constexpr std::array<std::string_view, 6> resource_calls{
        "cache.finished", "cache.unfinished", "pages.free", "pages.flush", "pages.sort", "slots.free"
    };
    if (action == "segment") {
        const auto & owner = facts.at("owner").get_ref<const std::string &>();

        // Result reads, resource calls and publication are separate actions; the residual has only CPU work.
        if (owner == "result") {
            facts.erase("entry");
            facts.erase("exit");
        }

        // Both native Decode paths have the same coarse tensor ABI; logical members retain the actual page demand.
        if (owner == "allocation") {
            auto & allocation = facts.at("allocation");
            if (allocation == "decode_tail" || allocation == "decode_pages") allocation = "decode_paged";
        }

        // These resource ABIs consume request/pool inputs, not the caller batch's forward mode.
        if (std::ranges::find(resource_calls, owner) != resource_calls.end()) facts.erase("phase");
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
    if (environment.at("implementation").at("framework_revision").get_ref<const std::string &>().empty())
        throw std::invalid_argument("SGLang implementation domain requires its upstream revision");
    auto result = environment;

    // Placement and initial state are target bindings; implementation and worker layout still require matching evidence.
    for (const auto field : { "device_index", "initial_state", "closure" }) result.erase(field);
    result.at("parallel").erase("rank");
    return result;
}

nlohmann::json NativeTemplates::share_batch_transforms(std::map<std::size_t, TemplateLibrary> & libraries) {
    struct Candidate {
        std::size_t source;
        std::size_t entry;
        TemplateEntry call;
        TemplateDefinition body;
    };
    std::map<nlohmann::json, std::vector<Candidate>> candidates;
    std::map<std::size_t, nlohmann::json> domains;
    std::map<std::size_t, std::vector<std::size_t>> resources;
    for (const auto & [index, library] : libraries) {
        auto environment = library.domain;

        // Closed batch arrays use member counts, not model shards or KV geometry; import_leaf checks their complete boundary.
        for (const auto field : { "worker_roles", "kv_peers", "parallel", "kv_layout", "expert_layers" }) environment.erase(field);
        environment.at("implementation").erase("disaggregation_mode");
        environment.at("implementation").erase("expert_layout");
        domains.emplace(index, environment);
        resources.emplace(index, batch_resources(library));
        for (std::size_t entry = 0; entry < library.entries.size(); ++entry) {
            const auto & call = library.entries[entry];
            if (call.action != "batch_transform") continue;
            const auto & transform = call.condition.at("transformation");
            if (!((transform.at("operation") == "merge" && transform.at("effect") == "merge")
                  || (transform.at("operation") == "filter" && transform.at("effect") == "subset")))
                continue;
            candidates[environment].push_back({ index, entry, call, library.definitions.at(call.frame.definition) });
        }
    }

    auto imports = nlohmann::json::array();
    for (auto & [index, library] : libraries) {
        const auto found = candidates.find(domains.at(index));
        if (found == candidates.end()) continue;
        for (const auto & candidate : found->second) {
            if (candidate.source == index) continue;
            if (const auto entry = library.import_leaf(candidate.body, candidate.call, resources.at(index)))
                imports.push_back({ { "recipient", index }, { "entry", *entry }, { "source", candidate.source }, { "source_entry", candidate.entry } });
        }
    }
    return imports;
}

} // namespace markov::sglang
