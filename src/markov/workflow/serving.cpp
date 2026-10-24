#include "markov/workflow/serving.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace markov {

ServingWorkflow::ServingWorkflow(ServingSettings settings, std::vector<Request> requests, BatchExpansion & expansion, const NodeDurations & durations)
    : settings_(settings),
      controller_(settings, std::move(requests)),
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
    BatchEffects effects;
    BatchProgram program{};
    nlohmann::json decision;

    while (!controller_.done()) {
        if (auto batch = controller_.plan(executor_.now_ns(), expansion_.admission())) {
            effects = controller_.effects(*batch);
            if (details) decision = describe_batch(*batch);

            // Construction and duration resolution succeed before request/page ownership changes.
            program = expansion_.expand(*batch, effects);
            executor_.submit(expansion_.graph(), program);
            controller_.start(std::move(*batch));
        }

        // Native control finishes in the idle interval, rather than after the next arrival.
        if (!executor_.has_events()) {
            auto ready_ns = executor_.now_ns();
            const auto completions = expansion_.idle();
            if (!completions.empty()) {
                ready_ns = executor_.evaluate(expansion_.graph(), completions);
                if (details)
                    result["controls"].push_back({
                        { "scheduled_ns", executor_.now_ns() },
                        { "completed_ns",           ready_ns }
                    });
                if (!details) executor_.retain(expansion_.retire());
            }

            // Requests arriving while control is busy participate in its next scheduling decision.
            executor_.advance_to(std::max(ready_ns, controller_.next_arrival_ns(executor_.now_ns()).value()));
            continue;
        }

        const auto event = executor_.next_event();
        if (event.kind == ExecutionEvent::Kind::Published) {
            record_publication(result, effects, event.time_ns, details);
            if (details) decision["published_ns"] = event.time_ns;
            continue;
        }

        // Publication does not release pages; result-processing completion commits those effects.
        controller_.finish(effects, program.returned_pages, event.time_ns);
        record_completion(result, decision, effects, event.time_ns, details);
        if (details) result["decisions"].push_back(std::move(decision));
        else executor_.retain(expansion_.retire());
    }

    result["control_status"] = "complete";
    result["free_pages"] = controller_.free_pages();
    result["completed_ns"] = executor_.now_ns();
    return result;
}

nlohmann::json ServingWorkflow::describe_batch(const Batch & batch) const {
    nlohmann::json decision{
        {             "phase", batch.phase == Phase::Prefill ? "prefill" : "decode" },
        {      "scheduled_ns",                                   batch.scheduled_ns },
        {                 "M",                                   batch.new_tokens() },
        {                 "B",                                 batch.members.size() },
        {                 "R",                       settings_.max_running_requests },
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
