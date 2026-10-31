#include "markov/controller/native.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace markov {

NativeController::NativeController(std::vector<Request> requests, std::span<const NativeInstance> instances, ControlPeer & peer,
                                   const nlohmann::json & prefixes, std::span<const NativeTransferRoute> transfers)
    : requests_(std::move(requests)),
      peer_(peer) {
    auto configuration = nlohmann::json::array();
    for (const auto & instance : instances) {
        inputs_.try_emplace(instance.id);
        auto bindings = nlohmann::json::array();
        for (const auto & participant : instance.participants) {
            groups_.emplace(participant.group,
                            std::make_unique<NativeGroup>(requests_, participant.resources, participant.group, participant.stage, instance.role,
                                                          instance.participants.size() > 1));
            instances_.emplace(participant.group, instance.id);
            bindings.push_back({
                {    "id", participant.group },
                { "stage", participant.stage }
            });
        }
        const auto & settings = instance.settings;
        configuration.push_back({
            {       "id",    instance.id                         },
            {   "groups",                     std::move(bindings) },
            { "settings",
             { { "max_running_requests", settings.max_running_requests },
             { "max_total_tokens", settings.max_total_tokens },
             { "page_size", settings.page_size },
             { "max_prefill_tokens", settings.max_prefill_tokens },
             { "prefill_chunk_tokens", instance.prefill_chunk_tokens },
             { "mixed_batches", instance.mixed_batches },
             { "expert_routes", instance.expert_routes },
             { "microbatch_requests", instance.batch_limit } }   }
        });
    }
    if (!transfers.empty()) transfer_ = std::make_unique<NativeTransfer>(transfers);
    auto content = nlohmann::json::array();
    for (const auto & request : requests_) {
        nlohmann::json value{
            {            "id",            request.id },
            { "prompt_tokens", request.prompt_tokens },
            { "output_tokens", request.output_tokens }
        };
        if (request.prefix_id) value["prefix_id"] = *request.prefix_id;
        content.push_back(std::move(value));
    }
    peer_.send({
        {      "kind",                                                      "configure" },
        { "instances",                                         std::move(configuration) },
        {  "workload", { { "requests", std::move(content) }, { "prefixes", prefixes } } }
    });
    for (;;) {
        const auto call = receive_call();
        const auto & message = call.at("message");
        if (message.at("kind") == "ready") break;
        auto & group = *groups_.at(message.at("group").get<std::string>());
        reply(call.at("id").get<std::size_t>(), group.resource_call(message, 0));
    }
}

nlohmann::json NativeController::receive_call() {
    peer_.send({
        { "kind", "poll" }
    });
    return peer_.receive();
}

void NativeController::reply(std::size_t call, const nlohmann::json & result) {
    peer_.send({
        {  "reply",   call },
        { "result", result }
    });
}

std::optional<ServingAction> NativeController::plan(std::int64_t now_ns, const GroupAdmission &) {
    while (!done()) {
        if (transfer_) {
            if (auto action = transfer_->plan(now_ns)) return action;
        }
        if (!shutdown_ && idle_.size() == groups_.size() && std::ranges::all_of(groups_, [](const auto & entry) { return entry.second->done(); })
            && (!transfer_ || transfer_->settled())) {

            // Every group finishes native batch cleanup before leaving at its control receive boundary.
            peer_.send({
                { "kind", "shutdown" }
            });
            for (auto & [_, input] : inputs_) input.call.reset();
            shutdown_ = true;
        }
        for (auto & [instance, input] : inputs_) {
            if (!input.call) continue;
            const bool arrived = input.arrived < requests_.size() && requests_[input.arrived].arrival_ns <= now_ns;

            // Empty rounds can carry downstream control messages; only the whole instance can be idle.
            const bool idle = std::ranges::all_of(groups_, [&](const auto & entry) {
                return instances_.at(entry.first) != instance || (entry.second->caught_up(input.arrived) && idle_.contains(entry.first));
            });

            // With no graph work in flight, native PP consensus still needs empty receive rounds.
            const bool executing = !running_calls_.empty() || (transfer_ && transfer_->running());
            if (arrived || (!idle && (!input.sequence || *input.sequence != sequence_ || !executing))) {
                auto arrivals = nlohmann::json::array();
                while (input.arrived < requests_.size() && requests_[input.arrived].arrival_ns <= now_ns) { arrivals.push_back(input.arrived++); }
                if (arrived) {
                    for (const auto & [group, owner] : instances_) {
                        if (owner == instance) idle_.erase(group);
                    }
                }
                reply(*input.call,
                      {
                          {     "kind",           "advance" },
                          {   "now_ns",              now_ns },
                          { "arrivals", std::move(arrivals) }
                });
                input.call.reset();
                input.sequence = sequence_;
            }
        }
        const auto call = receive_call();
        if (call.at("kind") == "waiting") return std::nullopt;
        const auto & message = call.at("message");
        const auto identity = call.at("id").get<std::size_t>();
        const auto kind = message.at("kind").get<std::string>();
        const auto name = message.at("group").get<std::string>();
        auto & input = inputs_.at(instances_.at(name));
        if (kind == "input") {
            input.call = identity;
            continue;
        }
        auto & group = *groups_.at(name);
        if (kind == "stopped") {
            if (!group.settled()) throw std::invalid_argument("native participant stopped with unfinished requests or work");
            stopped_.insert(name);
            continue;
        }
        if (kind == "begin") {
            group.begin_iteration(message.at("flow").get<std::size_t>(), message.at("lane").get<std::size_t>());
            continue;
        }
        if (kind == "idle") {
            idle_.insert(name);
            continue;
        }
        if (kind.starts_with("transfer_") && kind != "transfer_submit") {
            reply(identity, transfer_->call(message));
            if (kind != "transfer_poll") ++sequence_;
            continue;
        }
        if (auto action = group.plan(message, now_ns, input.arrived)) {
            if (kind == "transfer_submit") transfer_->submit(*action, message.at("payload"));
            idle_.erase(name);
            running_calls_.emplace(action->task, identity);
            return action;
        }
        reply(identity, group.resource_call(message, input.arrived));
    }
    return std::nullopt;
}

void NativeController::start(const ServingAction & action) {
    if (action.step != ServingStep::Transfer) groups_.at(action.task.group)->start(action);
    ++sequence_;
}

void NativeController::finish(const ServingAction & action, const BatchProgram & program, std::int64_t completed_ns) {
    if (transfer_) transfer_->finish(action);
    ++sequence_;
    if (action.step == ServingStep::Transfer) return;
    auto & group = *groups_.at(action.task.group);
    group.finish(action, program.returned_pages);
    reply(running_calls_.at(action.task),
          {
              { "completed_ns",   completed_ns },
              {       "values", program.values }
    });
    running_calls_.erase(action.task);
}

std::optional<std::int64_t> NativeController::next_arrival_ns(std::int64_t now_ns) const {
    std::optional<std::int64_t> next;
    for (const auto & [_, input] : inputs_) {
        if (input.arrived == requests_.size()) continue;
        const auto arrival = std::max(now_ns, requests_[input.arrived].arrival_ns);
        if (!next || arrival < *next) next = arrival;
    }
    return next;
}

bool NativeController::done() const { return stopped_.size() == groups_.size() && running_calls_.empty(); }
std::size_t NativeController::free_pages() const {
    std::size_t pages = 0;
    for (const auto & [name, group] : groups_) pages += group->free_pages();
    return pages;
}
std::size_t NativeController::request_count() const { return requests_.size(); }
const Request & NativeController::request(std::size_t identity) const { return requests_.at(identity); }

} // namespace markov
