#include "markov/controller/native.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace markov {

NativeController::NativeController(ServingSettings settings, std::vector<Request> requests, std::span<const NativeParticipant> participants,
                                   std::size_t batch_limit, ControlPeer & peer)
    : requests_(std::move(requests)),
      input_group_(participants.front().group),
      peer_(peer) {
    auto bindings = nlohmann::json::array();
    for (const auto & participant : participants) {
        groups_.emplace(participant.group, std::make_unique<NativeGroup>(requests_, participant.resources, participant.group, participant.stage));
        bindings.push_back({
            {    "id", participant.group },
            { "stage", participant.stage }
        });
    }
    peer_.send({
        {     "kind","configure"                     },
        {   "groups",            std::move(bindings) },
        { "settings",
         { { "max_running_requests", settings.max_running_requests },
         { "max_total_tokens", settings.max_total_tokens },
         { "page_size", settings.page_size },
         { "max_prefill_tokens", settings.max_prefill_tokens },
         { "microbatch_requests", batch_limit } }   }
    });
    for (;;) {
        const auto call = receive_call();
        const auto & message = call.at("message");
        if (message.at("kind") == "ready") break;
        auto & group = *groups_.at(message.at("group").get<std::string>());
        reply(call.at("id").get<std::size_t>(), group.resource_call(message, arrived_));
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
        if (!shutdown_ && (groups_.size() == 1 || idle_.size() == groups_.size())
            && std::ranges::all_of(groups_, [](const auto & entry) { return entry.second->done(); })) {

            // Workers leave at native input boundaries, then consume their remaining send handles before stopping.
            peer_.send({
                { "kind", "shutdown" }
            });
            input_call_.reset();
            shutdown_ = true;
        }
        if (input_call_) {
            const bool arrived = arrived_ < requests_.size() && requests_[arrived_].arrival_ns <= now_ns;
            const bool drained = groups_.size() > 1 && groups_.at(input_group_)->done() && idle_.contains(input_group_);
            if (!drained && (!idle_sequence_ || *idle_sequence_ != sequence_ || arrived)) {
                auto arrivals = nlohmann::json::array();
                while (arrived_ < requests_.size() && requests_[arrived_].arrival_ns <= now_ns) {
                    const auto & input = requests_[arrived_];
                    arrivals.push_back({
                        {       "request",          arrived_++ },
                        { "prompt_tokens", input.prompt_tokens },
                        { "output_tokens", input.output_tokens }
                    });
                }
                reply(*input_call_,
                      {
                          {     "kind",           "advance" },
                          {   "now_ns",              now_ns },
                          { "arrivals", std::move(arrivals) }
                });
                input_call_.reset();
                idle_sequence_.reset();
                input_sequence_ = sequence_;
            }
        }
        const auto call = receive_call();
        if (call.at("kind") == "waiting") return std::nullopt;
        const auto & message = call.at("message");
        const auto identity = call.at("id").get<std::size_t>();
        const auto kind = message.at("kind").get<std::string>();
        if (kind == "input") {
            input_call_ = identity;
            continue;
        }
        const auto name = message.at("group").get<std::string>();
        auto & group = *groups_.at(name);
        if (kind == "stopped") {
            if (!group.settled()) throw std::invalid_argument("native participant stopped with unfinished requests or work");
            stopped_.insert(name);
            continue;
        }
        if (kind == "begin") {
            idle_.erase(name);
            group.begin_iteration(message.at("flow").get<std::size_t>(), message.at("lane").get<std::size_t>());
            continue;
        }
        if (kind == "idle") {
            idle_.insert(name);

            // A no-model round can still release resources; it must not hide progress since its input poll.
            if (name == input_group_) idle_sequence_ = input_sequence_;
            continue;
        }
        if (auto action = group.plan(message, now_ns, arrived_)) {
            running_calls_.emplace(action->task, identity);
            return action;
        }
        reply(identity, group.resource_call(message, arrived_));
    }
    return std::nullopt;
}

void NativeController::start(const ServingAction & action) {
    groups_.at(action.task.group)->start(action);
    ++sequence_;
}

void NativeController::finish(const ServingAction & action, const BatchProgram & program, std::int64_t completed_ns) {
    auto & group = *groups_.at(action.task.group);
    group.finish(action, program.returned_pages);
    reply(running_calls_.at(action.task),
          {
              { "completed_ns",   completed_ns },
              {         "done",   group.done() },
              {       "values", program.values }
    });
    running_calls_.erase(action.task);
}

std::optional<std::int64_t> NativeController::next_arrival_ns(std::int64_t now_ns) const {
    if (arrived_ == requests_.size()) return std::nullopt;
    return std::max(now_ns, requests_[arrived_].arrival_ns);
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
