#include "markov/serving/session.hpp"

#include "markov/serving/execution_inputs.hpp"
#include "markov/execution/communication.hpp"
#include "markov/execution/timing.hpp"
#include "markov/performance/work_costs.hpp"
#include "markov/templates/library.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
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

nlohmann::json ServingSession::run(ServiceDuration given_work_ns, ExecutionInputs * inputs) {
    CommunicationLinks communication;
    std::map<std::string, GraphDependencies *> pools;
    for (auto & [group, participant] : participants_) pools.emplace(group, &participant.dependencies);
    struct ReturnedAction {
        ServingAction action;
        std::map<std::string, StorageBinding> values;
        std::size_t caller_returned;
    };
    std::map<std::size_t, ReturnedAction> pending;
    std::map<std::string, std::size_t> actions;
    std::size_t event_count = 0;
    std::size_t cost_queries = 0;
    std::chrono::nanoseconds cost_query_time{};
    std::vector<TemplateInterval> origins;
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
            const auto supplied = inputs
                ? std::optional(inputs->invocation(action->task.group, "action", ExecutionInputs::action_identity(*action)))
                : std::nullopt;
            const auto bindings = supplied && !inputs->full_coverage() ? supplied->bindings() : action->work.value("bindings", nlohmann::json::object());
            const auto graph = participant.graphs.build(*action, bindings, participant.costs ? &origins : nullptr);

            // Resolve all service times before this action can commit any events or dependency bindings.
            GraphTiming timing;
            if (supplied) {
                timing = supplied->timing(graph);
            } else if (participant.costs) {
                const auto began = std::chrono::steady_clock::now();
                timing = participant.costs->timing(graph, origins);
                cost_query_time += std::chrono::steady_clock::now() - began;
                cost_queries += graph.work.size();
            } else {
                timing.work_ns.assign(graph.work.size(), given_work_ns);
            }
            const auto events = participant.dependencies.insert(graph, timeline_, timing.work_ns, pools, timing.boundaries);
            communication.insert(graph, events, timeline_);
            const auto returned = events.at(graph.scopes.front().port("call.return", Direction::Output).event);
            auto caller_returned = returned;
            for (const auto & port : graph.scopes.front().ports) {
                if (port.name == "caller.return" && port.direction == Direction::Output) caller_returned = events.at(port.event);
            }
            ++actions[action->name];
            std::map<std::string, StorageBinding> values;
            for (const auto & port : graph.scopes.front().ports) {
                if (port.use == PortUse::Data && port.direction == Direction::Output)
                    values.emplace(port.name, StorageBinding{ port.tensor->storage, port.tensor->version });
            }
            pending.emplace(returned, ReturnedAction{ std::move(*action), std::move(values), caller_returned });
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
        control_.finish(action, completion->event, returned->second.caller_returned, completion->time_ns, returned->second.values);
        pending.erase(returned);
        plan = true;
    }
    if (!communication.empty()) throw std::runtime_error("serving ended with missing communication participants");
    nlohmann::json result{
        {     "completed_ns",       timeline_.now_ns() },
        {          "actions",                  actions },
        { "completed_events",              event_count },
        {   "work_intervals",     timeline_.work_count() },
        { "communication_payloads", communication.payload_coverage() },
        {         "requests", control_.request_count() },
        {       "free_pages",    control_.free_pages() },
        {          "outputs",       std::move(outputs) }
    };
    if (inputs) {
        result.update(inputs->finish());
    } else if (given_work_ns) {
        result.update({ { "duration_basis", "uniform_given_work_ns" }, { "given_work_ns", *given_work_ns } });
    } else {
        result.update({ { "duration_basis", "independent_service_costs" }, { "cost_queries", cost_queries },
                        { "cost_query_ns", cost_query_time.count() } });
    }
    return result;
}

} // namespace markov
