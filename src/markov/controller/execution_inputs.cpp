#include "markov/controller/execution_inputs.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "markov/controller/observer.hpp"
#include "markov/graph/graph.hpp"

namespace markov {

ExecutionInputs::ExecutionInputs(const nlohmann::json & source, ControlObserver * observer) : observer_(observer) {
    if (!source.at("actors").is_object()) throw std::invalid_argument("execution inputs require an actor mapping");
    for (const auto & [field, _] : source.items()) {
        if (field != "actors" && field != "notifications" && field != "transfers" && field != "ingress")
            throw std::invalid_argument("unknown execution input field: " + field);
    }
    for (const auto & [group, record] : source.at("actors").items()) {
        const auto & steps = record.at("steps");
        if (group.empty() || !steps.is_array() || record.size() != 1)
            throw std::invalid_argument("execution inputs require named actors containing only ordered operation arrays");
        for (const auto & step : steps) {
            const auto operation = step.at("operation").get<std::string>();
            if ((operation != "action" && operation != "cpu" && operation != "input") || !step.at("identity").is_object()
                || !step.at("source").is_object() || step.at("source").empty())
                throw std::invalid_argument("execution input requires an operation, logical identity and source reference");
        }
        actors_.emplace(group, Actor{ steps });
    }
    if (actors_.empty()) throw std::invalid_argument("execution inputs require participants");
    for (const auto & record : source.at("notifications")) {
        const auto key = notification_identity(record.at("identity"));
        window(record.at("window"));
        if (!notifications_.emplace(key, Condition{ record.at("window") }).second)
            throw std::invalid_argument("execution inputs repeat a notification");
    }
    for (const auto & record : source.at("transfers")) {
        const auto & posts = record.at("posts");
        if (!posts.is_array() || posts.empty() || posts.size() > 2)
            throw std::invalid_argument("CPU transfer requires its observed in-flow post positions");
        std::set<nlohmann::json> positions;
        for (const auto & post : posts) {
            const auto & actor = actors_.at(post.at("group").get<std::string>());
            const auto index = read_parameters({{ "step", post.at("step") }}).at("step");
            if (index < 0 || static_cast<std::size_t>(index) >= actor.steps.size() || actor.steps[static_cast<std::size_t>(index)].at("operation") != "cpu"
                || !positions.insert(post).second)
                throw std::invalid_argument("CPU transfer refers to an invalid or repeated post");
            const auto operation = actor.steps[static_cast<std::size_t>(index)].at("identity").at("operation").get<std::string>();
            if (operation != "send" && operation != "recv" && operation != "isend" && operation != "irecv")
                throw std::invalid_argument("CPU transfer position does not refer to a post");
        }
        const nlohmann::json key = std::vector<nlohmann::json>(positions.begin(), positions.end());
        const auto duration = service(record);
        if (!record.at("window").is_null()) window(record.at("window"));
        else if (!duration) throw std::invalid_argument("unknown CPU transfer service needs a completion window");
        if (!transfers_.emplace(key, Condition{ record }).second) throw std::invalid_argument("execution inputs repeat a CPU transfer");
    }
}

void ExecutionInputs::bind(std::span<const std::string> participants) const {
    if (participants.size() != actors_.size() || std::ranges::any_of(participants, [&](const auto & group) { return !actors_.contains(group); }))
        throw std::invalid_argument("execution input actors differ from the actual serving participants");
}

TimeWindow ExecutionInputs::window(const nlohmann::json & record) {
    const auto values = read_parameters(record);
    const TimeWindow result{ values.at("begin_ns"), values.at("end_ns") };
    if (values.size() != 2 || result.begin_ns < 0 || result.end_ns < result.begin_ns)
        throw std::invalid_argument("execution condition needs ordered nonnegative nanoseconds");
    return result;
}

ServiceDuration ExecutionInputs::service(const nlohmann::json & record) {
    if (record.at("service_ns").is_null()) return std::nullopt;
    const auto result = read_parameters({{ "service_ns", record.at("service_ns") }}).at("service_ns");
    if (result < 0) throw std::invalid_argument("execution service must be nonnegative nanoseconds or unknown");
    return result;
}

nlohmann::json ExecutionInputs::work_identity(const nlohmann::json & work) {
    auto members = work.value("members", nlohmann::json::array());
    for (auto & member : members) member.erase("slot");
    return {{ "members", std::move(members) }, { "context", work.at("context") },
             { "callers", work.value("callers", nlohmann::json::array()) },
             { "continuation", work.value("continuation", nlohmann::json()) },
             { "observation_context", work.value("observation_context", nlohmann::json()) }};
}

nlohmann::json ExecutionInputs::action_identity(const ServingAction & action) {
    return {{ "action", action.name }, { "parameters", action.parameters }, { "work", work_identity(action.work) }};
}

nlohmann::json ExecutionInputs::notification_identity(const nlohmann::json & value) {
    const auto group = value.at("group").get<std::string>();
    const auto name = value.at("name").get<std::string>();
    const auto & work = value.at("work");
    if (group.empty() || name.empty() || !work.is_object() || work.empty())
        throw std::invalid_argument("notification requires a participant, name and logical work identity");
    return {{ "group", group }, { "name", name }, { "work", work }};
}

const nlohmann::json & ExecutionInputs::take(const std::string & group, const std::string & operation, const nlohmann::json & identity) {
    auto & actor = actors_.at(group);
    const nlohmann::json actual{{ "operation", operation }, { "identity", identity }};
    if (actor.next == actor.steps.size())
        throw std::invalid_argument("execution input exhausted for " + group + " before " + actual.dump());
    const auto & expected = actor.steps[actor.next];
    if (expected.at("operation") != operation || expected.at("identity") != identity)
        throw std::invalid_argument("first execution input divergence: " + nlohmann::json{
            { "group", group }, { "step", actor.next }, { "expected", expected }, { "actual", actual }
        }.dump());
    if (observer_) observer_->condition_used(group, actor.next, operation, identity, expected.at("source"));
    ++actor.next;
    return expected;
}

std::size_t ExecutionInputs::boundary(const CoarseGraph & graph, const nlohmann::json & selector) {
    const auto & root = graph.scopes.front();
    const auto & identity = selector.contains("resource") ? selector.at("resource") : selector;
    if (identity.size() != 2 || (selector.contains("resource") && selector.size() != 1))
        throw std::invalid_argument("execution boundary requires a named port or a semantic resource interface");
    std::optional<std::size_t> resource;
    if (identity.contains("port")) {
        const auto & port = root.port(identity.at("port").get<std::string>(), direction_kind(identity.at("direction").get<std::string>()));
        if (!selector.contains("resource")) return port.event;
        resource = graph.events[port.event].resource;
    }

    // Resource tails are the invocation's declared interface, never a native stream/thread number.
    std::optional<std::size_t> result;
    for (const auto & port : root.ports) {
        if (!port.name.starts_with("resource.exit/")) continue;
        const auto index = graph.events[port.event].resource;
        if (!resource && std::ranges::any_of(root.ports, [&](const auto & named) {
                return !named.name.starts_with("resource.enter/") && !named.name.starts_with("resource.exit/") && graph.events[named.event].resource == index;
            }))
            continue;
        const auto & descriptor = graph.resources[index];
        const bool matches =
            resource ? *resource == index
                     : descriptor.kind == resource_kind(identity.at("kind").get<std::string>()) && descriptor.role == identity.at("role").get<std::string>();
        if (!matches) continue;
        if (result) throw std::invalid_argument("execution resource tail has an ambiguous semantic interface");
        result = port.event;
    }
    if (!result) throw std::invalid_argument("execution resource tail has no matching invocation interface");
    return *result;
}

InvocationInput ExecutionInputs::graph(const std::string & group, const std::string & operation, const nlohmann::json & identity, const CoarseGraph & graph) {
    const auto & record = take(group, operation, identity);
    std::map<std::size_t, TimeWindow> times;
    std::set<std::pair<std::string, Direction>> names;
    std::set<nlohmann::json> selectors;
    for (const auto & condition : record.at("boundaries")) {
        const auto & selector = condition.at("at");
        if (!selectors.insert(selector).second) throw std::invalid_argument("execution input repeats a boundary selector");
        if (selector.contains("port")) names.emplace(selector.at("port").get<std::string>(), direction_kind(selector.at("direction").get<std::string>()));
        const auto event = boundary(graph, selector);
        const auto value = window(condition.at("window"));
        const auto [found, inserted] = times.emplace(event, value);
        if (!inserted) {
            found->second.begin_ns = std::max(found->second.begin_ns, value.begin_ns);
            found->second.end_ns = std::min(found->second.end_ns, value.end_ns);
            if (found->second.begin_ns > found->second.end_ns) throw std::invalid_argument("aliased execution conditions conflict");
        }
    }
    if (!names.contains({ "call.enter", Direction::Input }) || !names.contains({ "call.return", Direction::Output }))
        throw std::invalid_argument("controlled invocation requires its actual CPU entry and return conditions");
    std::vector<BoundaryTime> boundaries;
    for (const auto & [event, value] : times) boundaries.push_back({ event, value });
    ++graph_count_;
    return {
        GraphTiming::conditioned(graph, std::vector<ServiceDuration>(graph.work.size(), service(record)), std::move(boundaries)),
        { group, actors_.at(group).next - 1 }
    };
}

std::optional<TimeWindow> ExecutionInputs::input(const std::string & group, const nlohmann::json & identity, bool in_flow) {
    const auto & actor = actors_.at(group);

    // Private empty polls before or after the observed serving window are not fabricated into measured operations.
    if (!in_flow && (actor.next == actor.steps.size() || actor.steps[actor.next].at("operation") != "input")) return std::nullopt;
    const auto & record = take(group, "input", identity);
    ++input_count_;
    return window(record.at("window"));
}

TimeWindow ExecutionInputs::notification(const nlohmann::json & identity) {
    auto & condition = notifications_.at(identity);
    if (std::exchange(condition.consumed, true)) throw std::invalid_argument("notification condition consumed twice: " + identity.dump());
    return window(condition.value);
}

EventTiming ExecutionInputs::transfer(std::span<const InputPosition> posts) {
    std::set<nlohmann::json> positions;
    for (const auto & post : posts) {
        if (actors_.at(post.group).next <= post.step) throw std::logic_error("CPU transfer precedes its checked input post");
        positions.insert(nlohmann::json{{ "group", post.group }, { "step", post.step }});
    }
    const nlohmann::json key = std::vector<nlohmann::json>(positions.begin(), positions.end());
    auto & condition = transfers_.at(key);
    if (std::exchange(condition.consumed, true)) throw std::invalid_argument("CPU transfer condition consumed twice: " + key.dump());
    const auto & record = condition.value;
    return { service(record), record.at("window").is_null() ? std::nullopt : std::optional(window(record.at("window"))) };
}

nlohmann::json ExecutionInputs::finish() const {
    for (const auto & [group, actor] : actors_) {
        if (actor.next != actor.steps.size())
            throw std::runtime_error("execution ended before input stream drained: " + group + " at " + std::to_string(actor.next));
    }
    for (const auto & conditions : { &notifications_, &transfers_ }) {
        for (const auto & [identity, condition] : *conditions) {
            if (!condition.consumed) throw std::runtime_error("execution ended with an unused completion condition: " + identity.dump());
        }
    }
    return {{ "duration_basis", "checked_causal_execution_inputs" }, { "timed_invocations", graph_count_ },
             { "timed_input_reads", input_count_ }, { "timed_notifications", notifications_.size() }, { "timed_cpu_transfers", transfers_.size() },
             { "timing_scope", "supplied_conditions_and_earliest_causal_realization_not_cost_accuracy" }};
}

} // namespace markov
