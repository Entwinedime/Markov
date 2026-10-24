#include "markov/workflow/serving.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov {

ServingWorkflow::ServingWorkflow(ServingControl & controller, BatchExpansion & expansion, const NodeDurations & durations)
    : controller_(controller),
      expansion_(expansion),
      executor_(durations) {}

nlohmann::json ServingWorkflow::run(bool details) {
    nlohmann::json result{
        {             "scope",     "serving_execution" },
        {            "status",               "partial" },
        { "completion_source",       "execution_ports" },
        {          "requests", nlohmann::json::array() }
    };

    // Accumulate user-visible events directly; retain per-token and per-batch histories only when requested.
    for (std::size_t identity = 0; identity < controller_.request_count(); ++identity) {
        const auto & request = controller_.request(identity);
        result["requests"].push_back({
            {            "id",            request.id },
            {    "arrival_ns",    request.arrival_ns },
            { "prompt_tokens", request.prompt_tokens },
            { "output_tokens", request.output_tokens }
        });
    }
    if (details) {
        result["decisions"] = nlohmann::json::array();
        result["outputs"] = nlohmann::json::array();
        result["controls"] = nlohmann::json::array();
    }

    struct PendingWork {
        ServingAction action;
        std::vector<std::size_t> returned_pages;
        nlohmann::json decision;
    };
    std::map<TaskIdentity, PendingWork> pending;

    while (!controller_.done() || !pending.empty()) {
        if (auto action = controller_.plan(executor_.now_ns(), expansion_.admission())) {
            nlohmann::json decision;
            if (details) {
                decision = describe_batch(action->batch);
                decision["task"] = {
                    {    "group",    action->task.group },
                    { "sequence", action->task.sequence }
                };
            }

            // Construction and duration resolution succeed before request/page ownership changes.
            auto program = expansion_.expand(*action);
            if (!program.publication && std::ranges::any_of(action->effects.members, &RequestCompletion::emits_output))
                throw std::invalid_argument("declared request output has no publication port");
            executor_.submit(expansion_.graph(), action->task, program);
            controller_.start(*action);

            // Keep the key before moving the snapshot; function argument evaluation must not move it first.
            const auto identity = action->task;
            pending.emplace(identity, PendingWork{ std::move(*action), std::move(program.returned_pages), std::move(decision) });
            continue;
        }

        // Native control finishes in the idle interval, rather than after the next arrival.
        const auto arrival_ns = controller_.next_arrival_ns(executor_.now_ns());
        if (pending.empty()) {
            if (!arrival_ns) throw std::logic_error("serving has no runnable work, completion or request arrival");
            const auto scheduled_ns = executor_.now_ns();
            const auto completions = expansion_.idle();
            const auto ready_ns = executor_.idle_until(expansion_.graph(), completions, *arrival_ns);
            if (!completions.empty()) {
                if (details)
                    result["controls"].push_back({
                        { "scheduled_ns", scheduled_ns },
                        { "completed_ns",     ready_ns }
                    });
                if (!details) executor_.retain(expansion_.retire());
            }
            continue;
        }

        // Progress internal work only as far as a visible completion or the next future arrival.
        const auto deadline_ns = arrival_ns && *arrival_ns > executor_.now_ns() ? arrival_ns : std::nullopt;
        const auto next = executor_.advance_until(deadline_ns);
        if (!next) continue;

        const auto & event = *next;
        auto & work = pending.at(event.task);
        if (event.kind == ExecutionEvent::Kind::Published) {
            record_publication(result, work.action.effects, event.time_ns, details);
            if (details) work.decision["published_ns"] = event.time_ns;
            continue;
        }

        // Publication does not release pages; result-processing completion commits those effects.
        controller_.finish(work.action, work.returned_pages, event.time_ns);
        record_completion(result, work.decision, work.action.effects, event.time_ns, details);
        if (details) result["decisions"].push_back(std::move(work.decision));
        pending.erase(event.task);

        // The executor keeps unfinished work and notifications; the expansion pins all live state bindings.
        if (!details) executor_.retain(expansion_.retire());
    }

    result["control_status"] = "complete";
    result["free_pages"] = controller_.free_pages();
    result["completed_ns"] = executor_.now_ns();
    if (details) result["execution_timeline"] = executor_.timeline();
    return result;
}

nlohmann::json ServingWorkflow::describe_batch(const Batch & batch) const {
    nlohmann::json decision{
        {             "phase", batch.phase == Phase::Prefill ? "prefill" : "decode" },
        {      "scheduled_ns",                                   batch.scheduled_ns },
        {                 "M",                                   batch.new_tokens() },
        {                 "B",                                 batch.members.size() },
        {                 "R",                               batch.request_capacity },
        { "free_pages_before",                              batch.free_pages_before },
        {   "allocated_pages",                                batch.allocated_pages },
        {           "members",                              nlohmann::json::array() }
    };
    for (const auto & member : batch.members) {
        decision["members"].push_back({
            {             "id",                                                 controller_.request(member.request).id },
            {           "slot",                                                                            member.slot },
            { "context_tokens",                                                                  member.context_tokens },
            {     "new_tokens",                                                                      member.new_tokens },
            {  "previous_page", member.previous_page ? nlohmann::json(*member.previous_page) : nlohmann::json(nullptr) },
            {      "new_pages",                                                                       member.new_pages }
        });
    }

    return decision;
}

void ServingWorkflow::record_publication(nlohmann::json & result, const BatchEffects & effects, std::int64_t time_ns, bool details) const {
    for (const auto & member : effects.members) {
        if (!member.emits_output) continue;

        auto & request = result["requests"][member.request];
        if (member.generated_tokens == 1) request["first_output_ns"] = time_ns;
        request["last_output_ns"] = time_ns;
        request["generated_tokens"] = member.generated_tokens;
        if (details) {
            result["outputs"].push_back({
                { "request",        request.at("id") },
                {   "token", member.generated_tokens },
                { "time_ns",                 time_ns }
            });
        }
    }
}

void ServingWorkflow::record_completion(nlohmann::json & result, nlohmann::json & decision, const BatchEffects & effects, std::int64_t time_ns,
                                        bool details) const {
    for (std::size_t index = 0; index < effects.members.size(); ++index) {
        const auto & member = effects.members[index];
        if (member.finished) result["requests"][member.request]["state_committed_ns"] = time_ns;
        if (details)
            decision["members"][index].update({
                { "generated_tokens", member.generated_tokens },
                {  "resident_tokens",  member.resident_tokens },
                {         "finished",         member.finished },
                {   "released_pages",   member.released_pages }
            });
    }

    if (details) {
        decision["completed_ns"] = time_ns;
        decision["free_pages_after"] = controller_.free_pages();
    }
}

} // namespace markov
