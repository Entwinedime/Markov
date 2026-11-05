#include "markov/serving/workflow.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace markov {

ServingWorkflow::ServingWorkflow(ServingControl & controller, BatchExpansion & expansion, ExecutionFeedback & execution, ResourceTrace * trace)
    : controller_(controller),
      expansion_(expansion),
      executor_(execution),
      trace_(trace) {}

nlohmann::json ServingWorkflow::run(bool details) {
    nlohmann::json result{
        {    "scope",     "serving_execution" },
        {   "status",               "partial" },
        { "requests", nlohmann::json::array() }
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
    }

    struct PendingWork {
        ServingAction action;
        BatchProgram program;
        nlohmann::json decision;
    };
    std::map<TaskIdentity, PendingWork> pending;

    // Diagnostics preserve state feedback order even when timestamps are equal.
    std::size_t state_order = 0;

    while (true) {
        if (trace_) trace_->context(executor_.now_ns(), state_order, "plan");
        if (auto action = controller_.plan(executor_.now_ns())) {
            if (pending.contains(action->task)) throw std::invalid_argument("control reused an in-flight task identity");

            // Resolve graph work before execution acceptance; a control backend may already hold its reservations.
            auto program = expansion_.expand(*action);
            if (program.completions.empty()) throw std::invalid_argument("work needs a completion port");
            const auto & graph = expansion_.graph();
            graph.require_bound_execution();
            const bool publishes = action->commits_requests() && std::ranges::any_of(action->batch_work().effects.members, &RequestCompletion::emits_output);
            if (program.publication.has_value() != publishes) throw std::invalid_argument("request output effects and publication ports disagree");
            auto decision = details ? describe_action(*action, program) : nlohmann::json{};
            executor_.submit(graph, *action, program);
            if (trace_) trace_->context(executor_.now_ns(), state_order, "start", action->task);
            controller_.start(*action);
            if (details) decision["submitted_order"] = state_order++;

            // Keep the key before moving the snapshot; function argument evaluation must not move it first.
            const auto identity = action->task;
            pending.emplace(identity, PendingWork{ std::move(*action), std::move(program), std::move(decision) });
            continue;
        }

        // Every graph operation belongs to a controller action, including final batch selection.
        const auto arrival_ns = controller_.next_arrival_ns(executor_.now_ns());
        if (pending.empty() && !arrival_ns && !controller_.done()) throw std::logic_error("serving has no runnable work, completion or request arrival");

        // With no future arrivals, this also drains submitted device tails after the last CPU feedback.
        const auto deadline_ns = arrival_ns && *arrival_ns > executor_.now_ns() ? arrival_ns : std::nullopt;
        const auto next = executor_.advance_until(deadline_ns);
        if (!next) {
            if (pending.empty() && !arrival_ns) break;
            continue;
        }

        const auto & event = *next;
        auto & work = pending.at(event.task);
        if (event.kind == ExecutionEvent::Kind::Published) {
            record_publication(result, work.action.batch_work().effects, event.time_ns, details);
            if (details) work.decision["published_ns"] = event.time_ns;
            continue;
        }

        // Publication does not release pages; result-processing completion commits those effects.
        if (trace_) trace_->context(event.time_ns, state_order, "complete", work.action.task);
        controller_.finish(work.action, work.program, event.time_ns);
        if (details) {
            work.decision["completed_ns"] = event.time_ns;
            work.decision["completed_order"] = state_order++;
            work.decision["free_pages_after"] = controller_.free_pages();
            work.decision["returned_pages"] = work.program.returned_pages;
        }
        if (details || work.action.commits_requests()) record_completion(result, work.decision, work.action, event.time_ns, details);
        if (details) result["decisions"].push_back(std::move(work.decision));
        pending.erase(event.task);

        // The executor keeps unfinished work and notifications; the expansion pins all live state bindings.
        if (!details) executor_.retain(expansion_.retire());
    }

    if (!expansion_.graph().execution.pending_communications().empty()) throw std::invalid_argument("serving finished with unmatched communication endpoints");
    result["control_status"] = "complete";
    result["completed_ns"] = executor_.now_ns();
    result.update(executor_.finish(details));
    return result;
}

nlohmann::json ServingWorkflow::describe_action(const ServingAction & action, const BatchProgram & program) const {
    nlohmann::json decision{
        {             "step",                                                                                action.step_name()                             },
        {     "scheduled_ns",                                                                                                            action.scheduled_ns },
        {             "flow",                                                                                                                    action.flow },
        {             "lane",                                                                                                                    action.lane },
        {            "stage",                                                         action.stage ? nlohmann::json(*action.stage) : nlohmann::json(nullptr) },
        { "commits_requests",                                                                                                      action.commits_requests() },
        {             "task",                                                       { { "group", action.task.group }, { "sequence", action.task.sequence } } },
        {  "execution_ports",
         { { "publication", program.publication ? nlohmann::json(*program.publication) : nlohmann::json(nullptr) }, { "completions", program.completions } } }
    };
    if (action.step == ServingStep::Schedule) {
        decision["operation"] = action.schedule_operation() == ScheduleOperation::Coordinate ? "coordinate" : "complete";
        return decision;
    }
    if (action.step == ServingStep::Poll) {
        decision["requests"] = nlohmann::json::array();
        for (const auto request : action.transfer_query().requests) decision["requests"].push_back(controller_.request(request).id);
        return decision;
    }
    if (const auto * endpoint = std::get_if<TransferEndpoint>(&action.work)) {
        decision["endpoint"] = {
            {   "group",                           endpoint->group },
            { "request", controller_.request(endpoint->request).id }
        };
        return decision;
    }
    if (action.step == ServingStep::Indices) {
        decision["index_operations"] = action.index_work().operations.size();
        return decision;
    }
    if (action.step == ServingStep::Select) {
        const auto & selection = action.selection();
        decision["view"] = selection.view == BatchView::Previous ? "previous" : "running";
        decision["members"] = nlohmann::json::array();
        for (const auto request : selection.requests)
            decision["members"].push_back({
                { "id", controller_.request(request).id }
            });
        return decision;
    }

    const auto & batch = action.batch_work().batch;
    decision.update({
        {                 "phase",     phase_name(batch.phase) },
        {                     "M",          batch.new_tokens() },
        {                     "B",        batch.members.size() },
        {                     "R",      batch.request_capacity },
        {     "free_pages_before",     batch.free_pages_before },
        { "deferred_pages_before", batch.deferred_pages_before },
        {       "allocated_pages",       batch.allocated_pages },
        {               "members",     nlohmann::json::array() }
    });
    for (const auto & member : batch.members) {
        decision["members"].push_back({
            {             "id",                                                 controller_.request(member.request).id },
            {          "phase",                                                               phase_name(member.phase) },
            {           "slot",                                                                            member.slot },
            { "context_tokens",                                                                  member.context_tokens },
            {     "new_tokens",                                                                      member.new_tokens },
            {  "previous_page", member.previous_page ? nlohmann::json(*member.previous_page) : nlohmann::json(nullptr) },
            {      "new_pages",                                                                       member.new_pages }
        });
    }

    if (!action.channel.empty()) decision["channel"] = action.channel;
    if (action.transfer)
        decision["transfer"] = {
            {      "source",      action.transfer->source },
            { "destination", action.transfer->destination }
        };
    if (action.step == ServingStep::Transfer || (action.step == ServingStep::Send && action.channel.empty()))
        decision["final_transfer"] = action.batch_work().final_transfer;
    if (!action.batch_work().transferred_pages.empty()) {
        decision["page_copies"] = nlohmann::json::array();
        for (const auto & page : action.batch_work().transferred_pages)
            decision["page_copies"].push_back({
                {      "source",      page.source },
                { "destination", page.destination }
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

void ServingWorkflow::record_completion(nlohmann::json & result, nlohmann::json & decision, const ServingAction & action, std::int64_t time_ns,
                                        bool details) const {
    const auto * work = std::get_if<std::shared_ptr<const BatchWork>>(&action.work);
    if (!work) return;
    const auto & effects = (*work)->effects;
    for (std::size_t index = 0; index < effects.members.size(); ++index) {
        const auto & member = effects.members[index];

        // Intermediate actions expose their planned effects without committing the external request state.
        if (action.commits_requests() && member.finished) result["requests"][member.request]["state_committed_ns"] = time_ns;
        if (details)
            decision["members"][index].update({
                {  "generated_tokens",  member.generated_tokens },
                {   "resident_tokens",   member.resident_tokens },
                { "continues_prefill", member.continues_prefill },
                {          "finished",          member.finished },
                {    "released_pages",    member.released_pages }
            });
    }
}

} // namespace markov
