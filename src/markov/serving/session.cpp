#include "markov/serving/session.hpp"

#include "markov/execution/communication.hpp"
#include "markov/controller/boundary_times.hpp"

#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace markov {

ServingSession::ServingSession(ServingControl & control, Timeline & timeline, std::map<std::string, ServingParticipant> participants)
    : control_(control),
      timeline_(timeline),
      participants_(std::move(participants)) {}

nlohmann::json ServingSession::run(std::int64_t given_work_ns, ControlBoundaryTimes * boundary_times) {
    if (given_work_ns < 0) throw std::invalid_argument("given work duration must be nonnegative nanoseconds");
    CommunicationLinks communication;
    std::map<std::string, GraphDependencies *> pools;
    for (auto & [group, participant] : participants_) pools.emplace(group, &participant.dependencies);
    struct ReturnedAction {
        ServingAction action;
        std::map<std::string, StorageBinding> values;
    };
    std::map<std::size_t, ReturnedAction> pending;
    std::map<std::string, std::size_t> actions;
    std::size_t event_count = 0;
    std::size_t work_count = 0;
    auto outputs = nlohmann::json::array();
    bool plan = true;
    while (!control_.done() || !timeline_.done()) {
        const bool planning = plan;
        while (plan) {
            auto action = control_.plan(timeline_.now_ns());
            if (!action) {
                plan = false;
                break;
            }
            auto & participant = participants_.at(action->task.group);
            const auto graph = participant.graphs.build(*action);
            const std::vector<std::int64_t> durations(graph.work.size(), given_work_ns);
            const auto boundaries = boundary_times ? boundary_times->take(*action, graph) : std::vector<BoundaryTime>{};
            const auto events = participant.dependencies.insert(graph, timeline_, durations, pools, boundaries);
            communication.insert(graph, events, timeline_);
            const auto returned = events.at(graph.scopes.front().port("call.return", Direction::Output).event);
            ++actions[action->name];
            work_count += graph.work.size();
            std::map<std::string, StorageBinding> values;
            for (const auto & port : graph.scopes.front().ports) {
                if (port.use == PortUse::Data && port.direction == Direction::Output)
                    values.emplace(port.name, StorageBinding{ port.tensor->storage, port.tensor->version });
            }
            pending.emplace(returned, ReturnedAction{ std::move(*action), std::move(values) });
        }

        if (planning) {

            // A consumer can retain another participant's signal or value; owners prune against all live references.
            std::set<std::string> bindings;
            for (const auto & [_, participant] : participants_) bindings.merge(participant.graphs.retained_bindings());
            auto retained = communication.retained_events();
            retained.merge(control_.retained_events());
            for (auto & [_, participant] : participants_) { retained.merge(participant.dependencies.retain(bindings, timeline_)); }
            timeline_.retain(std::move(retained));
        }
        const auto wakeup = control_.next_wakeup_ns(timeline_.now_ns());
        const auto completion = timeline_.advance(wakeup);
        if (wakeup && timeline_.now_ns() == *wakeup) plan = true;
        if (!completion) {
            if (wakeup) {
                plan = true;
                continue;
            }
            if (control_.done() && timeline_.done()) continue;
            throw std::runtime_error("serving stalled: no runnable event, future control boundary or native action");
        }
        ++event_count;
        if (control_.notify(completion->event)) plan = true;
        const auto returned = pending.find(completion->event);
        if (returned == pending.end()) continue;

        // CPU return releases only its native caller. Device tails remain in the same live timeline.
        const auto & action = returned->second.action;
        for (const auto & output : action.outputs) {
            if (!output.at("emits_output").get<bool>()) continue;
            outputs.push_back({
                {          "request", control_.request(output.at("request").get<std::size_t>()).id },
                { "generated_tokens",                                output.at("generated_tokens") },
                {         "finished",                                        output.at("finished") },
                {          "time_ns",                                          completion->time_ns }
            });
        }
        control_.finish(action, completion->event, completion->time_ns, returned->second.values);
        pending.erase(returned);
        plan = true;
    }
    if (!communication.empty()) throw std::runtime_error("serving ended with missing communication participants");
    nlohmann::json result{
        {   "duration_basis",  "uniform_given_work_ns" },
        {    "given_work_ns",            given_work_ns },
        {     "completed_ns",       timeline_.now_ns() },
        {          "actions",                  actions },
        { "completed_events",              event_count },
        {   "work_intervals",               work_count },
        { "communication_payloads", communication.payload_coverage() },
        {         "requests", control_.request_count() },
        {       "free_pages",    control_.free_pages() },
        {          "outputs",       std::move(outputs) }
    };
    if (boundary_times) result.update(boundary_times->finish());
    return result;
}

} // namespace markov
