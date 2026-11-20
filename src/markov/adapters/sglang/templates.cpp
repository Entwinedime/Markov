#include "markov/adapters/sglang/templates.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <optional>
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

using NativeOperation = std::pair<std::string, std::string>;

NativeOperation native_operation(const TemplateLibrary & library, const TemplateEntry & entry) {
    return { entry.action, library.definitions.at(entry.frame.definition).implementation };
}

bool batch_array(const TemplateEntry & entry) {
    if (entry.action != "batch_transform") return false;
    const auto & transform = entry.condition.at("transformation");
    return (transform.at("operation") == "merge" && transform.at("effect") == "merge")
        || (transform.at("operation") == "filter" && transform.at("effect") == "subset");
}

class ActionDeclarations {
public:
    using Lane = std::set<std::size_t>;

    explicit ActionDeclarations(const TemplateLibrary & library) : library_(library) {
        for (const auto & entry : library.entries) {
            operations_[native_operation(library, entry)].insert(entry.frame.resources.begin(), entry.frame.resources.end());
            const auto owner = entry.condition.value("owner", std::string{});
            if (entry.action == "batch_transform" || (entry.action == "segment" && (owner == "preparation" || owner == "allocation")))
                foreground_.insert(entry.frame.resources.begin(), entry.frame.resources.end());
        }
    }

    const Lane * operation(const NativeOperation & family) const {
        const auto found = operations_.find(family);
        return found == operations_.end() ? nullptr : &found->second;
    }

    const Lane & foreground() const { return foreground_; }

    std::optional<std::vector<std::size_t>> bind(const ActionDeclarations & source, const TemplateEntry & entry, const Lane & lane) const {
        const auto & source_lane = batch_array(entry) ? source.foreground_ : source.operations_.at(native_operation(source.library_, entry));
        const auto source_streams = std::ranges::count_if(source_lane, [&](auto index) {
            return source.library_.resources.at(index).kind == ResourceKind::Stream;
        });
        const auto target_streams = std::ranges::count_if(lane, [&](auto index) {
            return library_.resources.at(index).kind == ResourceKind::Stream;
        });
        std::vector<std::size_t> bindings;
        for (const auto original : entry.frame.resources) {
            const auto & expected = source.library_.resources.at(original);
            std::vector<std::size_t> matches;
            for (const auto index : lane) {
                const auto & actual = library_.resources.at(index);

                // One observed operation lane can have different native stream roles in ordinary and PP workers.
                const bool foreground = expected.kind == ResourceKind::Stream && source_streams == 1 && target_streams == 1;
                if (actual.kind == expected.kind && actual.serial == expected.serial && (actual.role == expected.role || foreground))
                    matches.push_back(index);
            }
            if (matches.size() != 1) return std::nullopt;
            bindings.push_back(matches.front());
        }
        return bindings;
    }

private:
    const TemplateLibrary & library_;
    std::map<NativeOperation, Lane> operations_;
    Lane foreground_;
};

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

    nlohmann::json segment(std::size_t scope, const nlohmann::json & segment) const {
        const auto & info = graph_.scopes[scope].info;
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
        if (action == "query") graph_.scopes[scope].port("caller.return", Direction::Output);
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

TemplateDeclarations NativeTemplates::describe(CoarseGraph graph) {
    const ActionConditions conditions(graph);
    std::vector<EntrySpecification> entries;
    std::map<std::size_t, std::map<std::string, std::size_t>> transfer_peers;

    // Read initialized parent implementations before deriving the parent's own expert-call binding.
    for (std::size_t scope = graph.scopes.size(); scope-- > 0;) {
        auto & observed = graph.scopes[scope];
        const auto & communication = observed.communication;
        if (observed.info.role == "expert_compute" || (communication && (communication->operation == "dispatch" || communication->operation == "combine"))) {
            const auto & owner = graph.scopes.at(observed.info.parent.value()).info;
            observed.info.bindings["local_implementation"] = owner.bindings.at("local_implementation");
        }
    }

    for (std::size_t scope = 0; scope < graph.scopes.size(); ++scope) {
        declare_payload_agreements(graph.scopes[scope]);
        auto & info = graph.scopes[scope].info;
        auto bindings = std::move(info.bindings);

        // Entry selection is separate from per-worker model and collective bindings.
        info.bindings = nlohmann::json::object();
        for (const auto name : { "weights", "collective_group", "local_implementation", "required_peers" }) {
            if (bindings.contains(name)) info.bindings[name] = std::move(bindings.at(name));
        }

        if (info.role == "kv_transfer") {
            std::set<std::string> peers;
            for (const auto & access : graph.scopes[scope].kv_accesses) {
                if (access.value.access == StateAccess::Write) peers.insert(access.value.pool);
            }
            for (const auto & access : graph.scopes[scope].slot_accesses) {
                if (access.value.access == StateAccess::Write) peers.insert(access.value.pool);
            }
            if (!peers.empty()) {
                if (peers.size() != 1) throw std::invalid_argument("native transport service requires one destination pool");
                auto & ordered = transfer_peers[info.parent.value()];
                const auto position = ordered.try_emplace(*peers.begin(), ordered.size()).first->second;
                info.bindings["state_input"] = std::string(graph.scopes[scope].kv_accesses.empty() ? "slot/" : "kv/") + std::to_string(position);
            }
        }

        // Native selection can run between PD caller segments while remaining an independently submitted action.
        const bool action = info.role == "segment" || bindings.value("action", false) || (info.parent == 0 && info.role != "invocation");
        if (scope == 0 || !action) continue;

        nlohmann::json condition{
            { "phase", info.work.value("phase", std::string("idle")) }
        };
        condition["state_accesses"] = conditions.bind_state(scope, info);
        if (bindings.contains("condition")) condition.update(bindings.at("condition"));
        if (info.role == "segment") condition.update(conditions.segment(scope, bindings.at("segment")));
        if (info.role == "signal_wait") {
            const bool device = bindings.at("device_wait").get<bool>();
            condition["device_wait"] = device;
            if (device) condition["stream"] = info.work.at("context").at("wait_stream").get<std::string>();
        }
        if (info.role == "signal_record") condition["stream"] = info.work.at("context").at("record_stream").get<std::string>();
        if (info.role == "stream_wait") condition["streams"] = bindings.at("streams");
        if (info.role == "gather") condition["collective"] = bindings.at("collective");
        if (info.role == "transfer_send" || info.role == "transfer_receive") condition["dtype"] = bindings.at("dtype");
        condition = NativeTemplates::condition(info.role, std::move(condition));
        conditions.validate(scope, info.role, condition);
        entries.push_back({ scope, info.role, std::move(condition) });
    }
    for (const auto & [scope, peers] : transfer_peers) {
        std::vector<std::string> order(peers.size());
        auto pools = nlohmann::json::object();
        for (const auto & [peer, position] : peers) {
            order[position] = peer;
            pools[peer] = peer;
        }

        // Local expansion receives endpoint identities and order, never target byte geometry or graph bodies.
        graph.scopes[scope].info.work["bindings"] = {{ "transport_order", order }};
        graph.scopes[scope].info.work["pools"] = std::move(pools);
    }
    return { std::move(graph), std::move(entries) };
}

TemplateLibrary NativeTemplates::compile(CoarseGraph graph, std::vector<TemplateWorkEvidence> * evidence) {
    const auto declarations = describe(std::move(graph));
    const auto & source = declarations.graph;
    auto library = TemplateCompiler::compile(source, declarations.entries, domain(source.environment), evidence);

    // All candidate libraries belong to one initialized model/layout; PD mode and capacity remain caller-owned.
    library.local_domain = nlohmann::json::object();
    for (const auto field : { "framework", "device", "torch", "torch_npu" }) library.local_domain[field] = source.environment.at(field);
    const auto & implementation = source.environment.at("implementation");
    for (const auto field : { "model", "dtype", "load_format", "framework_revision" }) library.local_domain[field] = implementation.at(field);
    library.local_domain["expert_layout"] = implementation.value("expert_layout", nlohmann::json());
    library.local_domain["expert_layers"] = source.environment.value("expert_layers", nlohmann::json::array());
    for (const auto field : { "logical_tp", "logical_pp", "logical_ep" }) library.local_domain[field] = source.environment.at("parallel").at(field);
    library.collect_local_implementations();
    library.validate();
    return library;
}

nlohmann::json NativeTemplates::condition(std::string_view action, nlohmann::json facts) {
    constexpr std::array<std::string_view, 7> resource_calls{
        "eviction", "cache.finished", "cache.unfinished", "pages.free", "pages.flush", "pages.sort", "slots.free"
    };

    // Scalar reads use their declared read contract; serving phase remains in the caller's work identity.
    if (action == "query" && facts.contains("read")) facts.erase("phase");
    if (action == "segment") {
        const auto & owner = facts.at("owner").get_ref<const std::string &>();

        // Result reads, resource calls and publication are separate actions; the residual has only CPU work.
        if (owner == "result") {
            facts.erase("entry");
            facts.erase("exit");
        }

        // Between prefix expressions only the Python comprehension advances; backend dispatch follows the loop.
        if (owner == "allocation" && facts.at("entry") == "allocation_prefix" && facts.at("exit") == "allocation_prefix")
            facts.erase("allocation");

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

nlohmann::json NativeTemplates::share_closed_actions(std::map<std::size_t, TemplateLibrary> & libraries) {
    struct Candidate {
        std::size_t source;
        std::size_t entry;
    };
    std::map<nlohmann::json, std::vector<Candidate>> candidates;
    std::map<std::size_t, nlohmann::json> domains;
    std::map<std::size_t, ActionDeclarations> declarations;
    for (const auto & [index, library] : libraries) {
        auto environment = library.domain;

        // The initialized implementation stays fixed; an operation's closed ABI qualifies reuse across worker layouts.
        for (const auto field : { "worker_roles", "kv_peers", "parallel", "kv_layout", "expert_layers" }) environment.erase(field);
        environment.at("implementation").erase("disaggregation_mode");
        environment.at("implementation").erase("expert_layout");
        domains.emplace(index, environment);

        // Imported entries cannot authorize more native methods or change the recipient's original lane declarations.
        declarations.try_emplace(index, library);
        for (std::size_t entry = 0; entry < library.entries.size(); ++entry) candidates[environment].push_back({ index, entry });
    }

    auto imports = nlohmann::json::array();
    for (auto & [index, library] : libraries) {
        const auto found = candidates.find(domains.at(index));
        if (found == candidates.end()) continue;
        const auto & declared = declarations.at(index);
        for (const auto & candidate : found->second) {
            if (candidate.source == index) continue;
            const auto & source = libraries.at(candidate.source);
            const auto & call = source.entries.at(candidate.entry);
            const auto & body = source.definitions.at(call.frame.definition);
            const auto * lane = declared.operation(native_operation(source, call));

            // Device-side layout stays worker-specific; declared member arrays and closed native CPU residuals can cross it.
            if (source.domain != library.domain && !batch_array(call)
                && (lane == nullptr || !std::ranges::all_of(body.work, [](const auto & work) { return work.kind == WorkKind::Host; })))
                continue;

            // A control-only observation of filter does not declare its tensor branch; member arrays use the foreground ABI.
            const auto & resources = !batch_array(call) && lane ? *lane : declared.foreground();
            const auto bindings = declared.bind(declarations.at(candidate.source), call, resources);
            if (!bindings) continue;
            if (const auto entry = library.import_leaf(body, call, *bindings))
                imports.push_back({ { "recipient", index }, { "entry", *entry }, { "source", candidate.source }, { "source_entry", candidate.entry } });
        }
    }
    return imports;
}

} // namespace markov::sglang
