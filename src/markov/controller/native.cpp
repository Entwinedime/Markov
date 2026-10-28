#include "markov/controller/native.hpp"

#include <algorithm>
#include <utility>

namespace markov {

NativeController::NativeController(ServingSettings settings, std::vector<Request> requests, KvPool & resources, std::string group, ControlPeer & peer)
    : requests_(std::move(requests)),
      group_(requests_, resources, std::move(group)),
      peer_(peer) {
    const nlohmann::json configuration{
        { "max_running_requests", settings.max_running_requests },
        {     "max_total_tokens",     settings.max_total_tokens },
        {            "page_size",            settings.page_size },
        {   "max_prefill_tokens",   settings.max_prefill_tokens }
    };
    peer_.send({
        {     "kind",   "configure" },
        { "settings", configuration }
    });

    for (;;) {
        const auto call = receive_call();
        const auto & message = call.at("message");
        if (message.at("kind") == "ready") break;
        reply(call.at("id").get<std::size_t>(), group_.resource_call(message, arrived_));
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
    if (done()) return std::nullopt;
    for (;;) {
        if (input_call_) {

            // An idle native iteration can finish cleanup; poll again only after actual work or a new arrival.
            const bool arrived = arrived_ < requests_.size() && requests_[arrived_].arrival_ns <= now_ns;
            if (input_sequence_ != group_.sequence() || arrived) {
                input_sequence_ = group_.sequence();
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
                group_.begin_iteration();
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
        if (kind == "idle") {
            group_.end_iteration();
            continue;
        }
        if (auto action = group_.plan(message, now_ns, arrived_)) {
            running_call_ = identity;
            return action;
        }
        reply(identity, group_.resource_call(message, arrived_));
    }
}

void NativeController::start(const ServingAction & action) { group_.start(action); }

void NativeController::finish(const ServingAction & action, std::span<const std::size_t> returned_pages, std::int64_t completed_ns) {
    group_.finish(action, returned_pages);
    reply(running_call_.value(),
          {
              { "completed_ns", completed_ns },
              {         "done",       done() }
    });
    running_call_.reset();
}

std::optional<std::int64_t> NativeController::next_arrival_ns(std::int64_t now_ns) const {
    if (arrived_ == requests_.size()) return std::nullopt;
    return std::max(now_ns, requests_[arrived_].arrival_ns);
}

bool NativeController::done() const { return group_.done(); }
std::size_t NativeController::free_pages() const { return group_.free_pages(); }
std::size_t NativeController::request_count() const { return requests_.size(); }
const Request & NativeController::request(std::size_t identity) const { return requests_.at(identity); }

} // namespace markov
