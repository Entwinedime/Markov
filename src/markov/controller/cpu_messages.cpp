#include "markov/controller/cpu_messages.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace markov {

CpuMessages::CpuMessages(Timeline & timeline, std::int64_t given_service_ns, ControlObserver * observer, ExecutionInputs * inputs)
    : timeline_(timeline), given_service_ns_(given_service_ns), observer_(observer), inputs_(inputs) {}

void CpuMessages::add_actor(const std::string & group, CpuActor actor) { actors_.emplace(group, std::move(actor)); }
void CpuMessages::prime() { priming_ = false; }

void CpuMessages::observe(const nlohmann::json & key, const Endpoint & endpoint, const nlohmann::json & fields) {
    if (!observer_) return;
    auto record = fields;
    record["rank"] = endpoint.actor;
    record["key"] = key;
    observer_->native_protocol(endpoint.group, record, timeline_.now_ns());
}

void CpuMessages::ensure_wire(const nlohmann::json & key, Message & message, const Endpoint & endpoint) {
    if (message.wire || message.ready || priming_) return;
    message.wire = timeline_.defer();
    completions_[*message.wire].push_back({ Phase::Wire, key, endpoint.descriptor.at("receiving").get<bool>() ? 1U : 0U, {} });
    observe(key, endpoint, {{ "phase", "wire" }, { "event", *message.wire }});
}

CpuMessages::Service CpuMessages::service(const nlohmann::json & key, const Endpoint & endpoint,
                                         const nlohmann::json & execution, std::optional<std::size_t> prerequisite) {
    const auto & resource = actors_.at(endpoint.group).control;
    ScopeInfo info{ {}, "cpu_service", "cpu_protocol", "cpu_protocol", {}, {}, {}, execution.at("work"),
                    {{ "cpu_protocol", {{ "key", key }, { "descriptor", endpoint.descriptor }, { "execution", execution }} }}, {} };
    CoarseGraph graph;
    graph.environment = nlohmann::json::object();
    graph.resources.push_back(resource);
    graph.scopes.push_back({ std::move(info), {
        { "call.enter", Direction::Input, PortUse::Control, Relation::Control, 0, {} },
        { "call.return", Direction::Output, PortUse::Control, Relation::Control, 1, {} }
    }});
    graph.events = {{ 0, 0 }, { 0, 0 }};
    graph.work = {{ 0, 0, WorkKind::Host, 0, 1 }};
    std::vector<ExecutionInput> inputs;

    // Blocking API time includes its wait, but only the local CPU service receives a duration.
    if (prerequisite) inputs.push_back({ *prerequisite, 1, Relation::Communication });
    GraphTiming timing{ { given_service_ns_ }, {} };
    std::optional<InputPosition> position;
    if (inputs_) {
        auto supplied = inputs_->graph(endpoint.group, "cpu", {
            { "operation", execution.at("operation") }, { "work", ExecutionInputs::work_identity(execution.at("work")) }
        }, graph);
        timing = std::move(supplied.timing);
        position = std::move(supplied.position);
    }
    const auto events = timeline_.insert(graph, timing.work_ns, inputs, timing.boundaries);
    ++service_count_;
    return { events[0], events[1], std::move(position) };
}

void CpuMessages::resolve(const nlohmann::json & key, Message & message) {
    if (message.resolved || !message.endpoints[0] || !message.endpoints[1]) return;
    auto & sender = *message.endpoints[0];
    const auto & receiver = *message.endpoints[1];
    if (sender.descriptor.at("dtype") != receiver.descriptor.at("dtype")
        || sender.descriptor.at("elements") != receiver.descriptor.at("elements")
        || sender.descriptor.at("element_bytes") != receiver.descriptor.at("element_bytes"))
        throw std::invalid_argument("CPU protocol endpoints disagree on wire geometry");

    // Unconsumed async tails expose no completion; materialize readiness when an actual consumer needs it.
    if (!priming_ && !message.wire && !sender.wait_call && !receiver.wait_call) return;

    message.resolved = true;
    if (priming_) {

        // Empty native startup establishes initial transport state, outside the modeled serving interval.
        message.ready = true;
        ++ready_count_;
        observe(key, sender, {{ "phase", "ready" }, { "event", nullptr }, { "prerequisites", nlohmann::json::array() }, { "service_ns", 0 }});
        for (auto & endpoint : message.endpoints) {
            if (endpoint->wait_call) consume(key, *endpoint);
        }
        return;
    }
    ensure_wire(key, message, sender);
    std::vector<std::size_t> prerequisites;
    for (const auto & endpoint : message.endpoints) {
        if (endpoint->entered) prerequisites.push_back(*endpoint->entered);
    }
    const bool in_flow = sender.execution.at("in_flow").get<bool>() || receiver.execution.at("in_flow").get<bool>();
    message.timing = { in_flow ? given_service_ns_ : 0, {} };
    if (in_flow && inputs_) {
        std::vector<InputPosition> positions;
        for (const auto & endpoint : message.endpoints) {
            if (endpoint->input) positions.push_back(*endpoint->input);
        }
        message.timing = inputs_->transfer(positions);
    }
    timeline_.resolve(*message.wire, prerequisites, Relation::Communication, message.timing.window, message.timing.service_ns);
}

void CpuMessages::post(std::size_t call, const std::string & group, const nlohmann::json & request) {
    const auto & key = request.at("key");
    const auto & descriptor = request.at("descriptor");
    const auto & execution = request.at("execution");
    if (!key.is_array() || key.size() != 5 || !execution.is_object() || !execution.at("in_flow").is_boolean()
        || !execution.at("work").is_object())
        throw std::invalid_argument("CPU post requires its route, native operation and entering execution context");
    const bool receiving = descriptor.at("receiving").get<bool>();
    const auto side = receiving ? 1U : 0U;
    const auto operation = execution.at("operation").get<std::string>();
    if ((receiving && operation != "recv" && operation != "irecv") || (!receiving && operation != "send" && operation != "isend")
        || key[0] != descriptor.at("group") || key[1] != descriptor.at("source") || key[2] != descriptor.at("target")
        || key[3] != descriptor.at("tag") || key[1] == key[2] || !key[4].is_number_integer() || key[4].get<std::int64_t>() < 0
        || descriptor.at("elements").get<std::int64_t>() < 0 || descriptor.at("element_bytes").get<std::int64_t>() <= 0)
        throw std::invalid_argument("CPU post has an inconsistent operation, route or wire geometry");
    const auto source = request.at("source").get<std::string>();
    const auto target = request.at("target").get<std::string>();
    if (source == target || group != (receiving ? target : source) || actors_.at(source).instance != actors_.at(target).instance)
        throw std::invalid_argument("CPU post crosses native worlds or changes its actor");
    auto & message = messages_[key];
    if (message.endpoints[side]) throw std::invalid_argument("CPU post repeats a live endpoint");
    auto & endpoint = message.endpoints[side].emplace(Endpoint{
        group, key[receiving ? 2 : 1].get<std::size_t>(), descriptor, execution, {}, {}, {}, {}, nullptr, {}, false
    });
    const bool in_flow = execution.at("in_flow").get<bool>();
    if (priming_ && in_flow) throw std::invalid_argument("native CPU startup cannot submit serving work");
    const bool blocking = operation == "send" || operation == "recv";
    if (in_flow) {
        if (blocking) ensure_wire(key, message, endpoint);
        const auto [entered, returned, position] = service(key, endpoint, execution, blocking ? message.wire : std::nullopt);
        endpoint.entered = entered;
        endpoint.returned = returned;
        endpoint.input = position;
        completions_[entered].push_back({ Phase::Post, key, side, blocking ? std::optional(call) : std::nullopt });
        if (!blocking) completions_[returned].push_back({ Phase::Return, key, side, call });
    }
    else {
        observe(key, endpoint, {{ "phase", "post" }, { "event", nullptr }, { "descriptor", descriptor }, { "execution", execution }});
        replies_.emplace_back(call, nlohmann::json{{ "key", key }});
    }
    resolve(key, message);
}

void CpuMessages::consume(const nlohmann::json & key, Endpoint & endpoint) {
    if (!endpoint.wait_call || endpoint.consumed) throw std::logic_error("CPU service released a consumer twice");
    endpoint.consumed = true;
    ++consumed_count_;
    observe(key, endpoint, {{ "phase", "consumed" }, { "event", endpoint.wait_return ? nlohmann::json(*endpoint.wait_return) : nlohmann::json(nullptr) },
                            { "execution", endpoint.wait_execution }});
    replies_.emplace_back(*endpoint.wait_call, nlohmann::json{{ "key", key }});
    endpoint.wait_call.reset();
}

void CpuMessages::wait(std::size_t call, const std::string & group, const nlohmann::json & request) {
    const auto & key = request.at("key");
    auto & message = messages_.at(key);
    const auto endpoint_it = std::ranges::find_if(message.endpoints, [&](const auto & endpoint) { return endpoint && endpoint->group == group; });
    if (endpoint_it == message.endpoints.end()) throw std::invalid_argument("CPU wait has no local post");
    auto & endpoint = **endpoint_it;
    const auto & execution = request.at("execution");
    const auto operation = endpoint.execution.at("operation").get<std::string>();
    const bool blocking = operation == "send" || operation == "recv";
    if (!execution.is_object() || execution.at("operation") != (blocking ? operation : operation + "_wait")
        || !execution.at("in_flow").is_boolean() || !execution.at("work").is_object() || endpoint.wait_call || endpoint.consumed)
        throw std::invalid_argument("CPU wait changes its operation or repeats a consumer");
    endpoint.wait_call = call;
    endpoint.wait_execution = execution;
    const bool in_flow = execution.at("in_flow").get<bool>();
    if (in_flow) {
        ensure_wire(key, message, endpoint);
        if (blocking) endpoint.wait_return = endpoint.returned;
        else endpoint.wait_return = service(key, endpoint, execution, message.wire).returned;
        if (!endpoint.wait_return) throw std::invalid_argument("blocking CPU service lost its entering execution interval");
        if (timeline_.completed(*endpoint.wait_return)) consume(key, endpoint);
        else completions_[*endpoint.wait_return].push_back({ Phase::Consume, key, static_cast<std::size_t>(endpoint_it - message.endpoints.begin()), {} });
    }
    resolve(key, message);
    if (!in_flow && message.ready && endpoint.wait_call) consume(key, endpoint);
}

void CpuMessages::call(std::size_t call, const std::string & group, const nlohmann::json & message) {
    if (message.at("kind") == "cpu_post") post(call, group, message);
    else wait(call, group, message);
}

void CpuMessages::notify(std::size_t event) {
    const auto found = completions_.find(event);
    if (found == completions_.end()) return;
    for (const auto & completion : found->second) {
        auto & message = messages_.at(completion.key);
        auto & endpoint = *message.endpoints[completion.endpoint];
        if (completion.phase == Phase::Post)
            observe(completion.key, endpoint, {{ "phase", "post" }, { "event", event }, { "descriptor", endpoint.descriptor }, { "execution", endpoint.execution }});
        else if (completion.phase == Phase::Wire) {
            message.ready = true;
            ++ready_count_;
            std::vector<std::size_t> prerequisites;
            for (auto & peer : message.endpoints) {
                if (peer->entered) prerequisites.push_back(*peer->entered);
            }
            observe(completion.key, endpoint, {{ "phase", "ready" }, { "event", event }, { "prerequisites", prerequisites },
                                              { "service_ns", message.timing.service_ns ? nlohmann::json(*message.timing.service_ns) : nlohmann::json(nullptr) }});
            for (auto & peer : message.endpoints) {
                if (peer->wait_call && !peer->wait_return) consume(completion.key, *peer);
            }
        }
        else if (completion.phase == Phase::Consume) consume(completion.key, endpoint);
        if (completion.call) replies_.emplace_back(*completion.call, nlohmann::json{{ "key", completion.key }});
    }
    completions_.erase(found);
}

std::vector<std::pair<std::size_t, nlohmann::json>> CpuMessages::take_replies() {
    auto replies = std::exchange(replies_, {});

    // Both Work handles have been consumed; later messages use the broker's next native channel position.
    std::erase_if(messages_, [](const auto & entry) {
        return std::ranges::all_of(entry.second.endpoints, [](const auto & endpoint) { return endpoint && endpoint->consumed; });
    });
    return replies;
}

std::set<std::size_t> CpuMessages::retained_events() const {
    std::set<std::size_t> events;
    for (const auto & [_, message] : messages_) {
        if (message.wire) events.insert(*message.wire);
        for (const auto & endpoint : message.endpoints) {
            if (!endpoint) continue;
            for (const auto event : { endpoint->entered, endpoint->returned, endpoint->wait_return }) {
                if (event) events.insert(*event);
            }
        }
    }
    return events;
}

bool CpuMessages::shutdown_ready() const {
    if (!completions_.empty()) return false;
    for (const auto & [_, message] : messages_) {
        for (const auto & endpoint : message.endpoints) {
            if (endpoint && endpoint->wait_call && endpoint->wait_execution.at("in_flow").get<bool>()) return false;
        }
    }
    return true;
}

nlohmann::json CpuMessages::summary() const {
    return {{ "cpu_protocol", {{ "service_intervals", service_count_ }, { "ready_messages", ready_count_ }, { "consumptions", consumed_count_ },
                               { "timing_basis", inputs_ ? "checked_execution_inputs" : "given_service_duration_outside_native_startup" }} }};
}

} // namespace markov
