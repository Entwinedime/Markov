#include "markov/controller/boundary_times.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov {

nlohmann::json ControlBoundaryTimes::identity(const std::string & group, const std::string & action, const Parameters & parameters,
                                              const nlohmann::json & work) {
    const auto & context = work.at("context");
    const auto callers = work.value("callers", nlohmann::json::array());
    const auto continuation = work.value("continuation", nlohmann::json());
    if (group.empty() || action.empty() || !context.is_object() || !callers.is_array()
        || (!continuation.is_null() && !continuation.is_object()))
        throw std::invalid_argument("boundary timing requires a group, action and logical work context");

    // Transport handles, physical pages and invocation ordinals are deliberately outside this identity.
    return { { "group", group }, { "action", action }, { "parameters", parameters },
             { "context", context }, { "callers", callers }, { "continuation", continuation } };
}

ControlBoundaryTimes::ControlBoundaryTimes(const nlohmann::json & source) {
    if (source.contains("actions")) {
        for (const auto & record : source.at("actions").get_ref<const nlohmann::json::array_t &>()) {
            const auto key = identity(record.at("group").get<std::string>(), record.at("action").get<std::string>(),
                                      read_parameters(record.at("parameters")), record.at("work"));
            const auto count = read_parameters({ { "count", record.at("observed_calls") } }).at("count");
            if (count < 1) throw std::invalid_argument("boundary timing requires a positive observed call count");

            std::vector<PortTime> boundaries;
            std::set<std::pair<std::string, Direction>> ports;
            for (const auto & value : record.at("boundaries").get_ref<const nlohmann::json::array_t &>()) {
                const auto name = value.at("port").get<std::string>();
                const auto direction = direction_kind(value.at("direction").get<std::string>());
                const auto time_ns = read_parameters({ { "time_ns", value.at("time_ns") } }).at("time_ns");
                if (time_ns < 0 || name.empty() || !ports.emplace(name, direction).second)
                    throw std::invalid_argument("boundary timing requires unique named ports and nonnegative nanoseconds: " + key.dump());
                boundaries.push_back({ name, direction, time_ns });
            }
            if (count > 1 && !boundaries.empty())
                throw std::invalid_argument("repeated calls cannot supply per-occurrence boundary times: " + key.dump());
            if (!timings_.emplace(key, ActionTimes{ std::move(boundaries), static_cast<std::size_t>(count) }).second)
                throw std::invalid_argument("ambiguous boundary timing work identity: " + key.dump());
        }
    }
    if (source.contains("notifications")) {
        for (const auto & record : source.at("notifications").get_ref<const nlohmann::json::array_t &>()) {
            const auto key = notification_identity(record.at("identity"));
            const auto values = read_parameters({ { "begin_ns", record.at("begin_ns") }, { "end_ns", record.at("end_ns") } });
            const TimeWindow window{ values.at("begin_ns"), values.at("end_ns") };
            if (window.begin_ns < 0 || window.end_ns < window.begin_ns || !notifications_.emplace(key, NotificationTime{ window }).second)
                throw std::invalid_argument("notification timing requires unique identities and ordered nonnegative intervals");
        }
    }
    if (source.contains("input_reads")) {
        for (const auto & record : source.at("input_reads")) {
            const auto & key = record.at("identity");
            const auto received = key.at("received").get<std::vector<std::string>>();
            if (key.at("instance").get<std::string>().empty() || !key.at("context").is_object()
                || !std::ranges::is_sorted(received) || std::ranges::adjacent_find(received) != received.end())
                throw std::invalid_argument("input timing requires an instance, native context and unique ordered received-request identities");
            const auto count = read_parameters({ { "observed_calls", record.at("observed_calls") } }).at("observed_calls");
            std::optional<std::int64_t> time;
            if (!record.at("time_ns").is_null()) time = read_parameters({ { "time_ns", record.at("time_ns") } }).at("time_ns");
            if (count < 1 || (time && (count != 1 || *time < 0)) || !inputs_.emplace(key, InputTime{ time, static_cast<std::size_t>(count) }).second)
                throw std::invalid_argument("input timing requires unique logical identities; repeated reads cannot supply exact times");
        }
    }
    if (notifications_.empty() && std::ranges::none_of(inputs_, [](const auto & entry) { return entry.second.time_ns.has_value(); })
        && std::ranges::none_of(timings_, [](const auto & entry) { return !entry.second.boundaries.empty(); }))
        throw std::invalid_argument("boundary times must contain measured action, notification or input boundaries");
}

nlohmann::json ControlBoundaryTimes::notification_identity(const nlohmann::json & value) {
    const auto group = value.at("group").get<std::string>();
    const auto name = value.at("name").get<std::string>();
    const auto & work = value.at("work");
    if (group.empty() || name.empty() || !work.is_object() || work.empty())
        throw std::invalid_argument("notification requires a participant, name and logical work identity");
    return { { "group", group }, { "name", name }, { "work", work } };
}

std::optional<TimeWindow> ControlBoundaryTimes::notification(const nlohmann::json & identity) {
    auto & timing = notifications_[identity];
    if (timing.consumed) throw std::invalid_argument("notification timing consumed twice: " + identity.dump());
    timing.consumed = true;
    return timing.window;
}

std::vector<BoundaryTime> ControlBoundaryTimes::take(const ServingAction & action, const CoarseGraph & graph) {
    const auto key = identity(action.task.group, action.name, action.parameters, action.work);

    // Sparse timing evidence cannot restrict the native controller's legal action paths.
    auto & timing = timings_.try_emplace(key, ActionTimes{ {}, 0 }).first->second;

    // Unknown or explicitly uncovered actions retain their graph and given durations.
    if (timing.boundaries.empty()) {
        ++timing.predicted_calls;
        return {};
    }
    if (timing.predicted_calls != 0) throw std::invalid_argument("boundary timing consumed twice: " + key.dump());
    std::map<std::size_t, std::int64_t> times;
    for (const auto & boundary : timing.boundaries) {
        const auto event = graph.scopes.front().port(boundary.port, boundary.direction).event;
        const auto [position, inserted] = times.emplace(event, boundary.time_ns);

        // Several exported names may denote the same boundary, but cannot assign it different times.
        if (!inserted && position->second != boundary.time_ns)
            throw std::invalid_argument("conflicting times for aliased action ports: " + key.dump());
    }
    std::vector<BoundaryTime> result;
    for (const auto & [event, time_ns] : times) result.push_back({ event, time_ns });
    ++timing.predicted_calls;
    ++actions_;
    events_ += result.size();
    return result;
}

std::optional<std::int64_t> ControlBoundaryTimes::input(const nlohmann::json & identity) {
    auto & timing = inputs_[identity];
    if (timing.time_ns && timing.predicted_calls != 0)
        throw std::invalid_argument("input timing consumed twice: " + identity.dump());
    ++timing.predicted_calls;
    return timing.time_ns;
}

nlohmann::json ControlBoundaryTimes::finish() const {
    auto unconstrained = nlohmann::json::array();
    for (const auto & [key, timing] : timings_) {
        if (timing.boundaries.empty() || timing.predicted_calls == 0)
            unconstrained.push_back({ { "identity", key }, { "observed_calls", timing.observed_calls },
                                      { "predicted_calls", timing.predicted_calls } });
    }
    auto uncovered_notifications = nlohmann::json::array();
    std::size_t notification_count = 0;
    for (const auto & [key, timing] : notifications_) {
        if (timing.window && timing.consumed) ++notification_count;
        else uncovered_notifications.push_back({ { "identity", key }, { "observed", timing.window.has_value() }, { "consumed", timing.consumed } });
    }
    auto uncovered_inputs = nlohmann::json::array();
    std::size_t input_count = 0;
    for (const auto & [key, timing] : inputs_) {
        if (timing.time_ns && timing.predicted_calls == 1) ++input_count;
        else uncovered_inputs.push_back({ { "identity", key }, { "observed_calls", timing.observed_calls }, { "predicted_calls", timing.predicted_calls } });
    }
    const auto coverage = unconstrained.empty() && uncovered_notifications.empty() && uncovered_inputs.empty() ? "complete" : "partial";
    return { { "duration_basis", "uniform_given_work_ns_with_boundary_times" }, { "boundary_actions", actions_ }, { "boundary_events", events_ },
             { "timing_coverage", coverage }, { "unconstrained_calls", std::move(unconstrained) },
             { "bounded_notifications", notification_count }, { "unconstrained_notifications", std::move(uncovered_notifications) },
             { "bounded_input_reads", input_count }, { "unconstrained_input_reads", std::move(uncovered_inputs) },
             { "notification_time_basis", "earliest_causal_time_within_observed_interval" } };
}

} // namespace markov
