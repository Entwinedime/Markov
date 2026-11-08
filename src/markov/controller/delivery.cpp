#include "markov/controller/delivery.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace markov {

RequestDelivery::RequestDelivery(std::span<const Request> requests, const nlohmann::json * ingress) : observed_(ingress != nullptr) {
    if (!observed_) {
        for (std::size_t index = 0; index < requests.size(); ++index) rounds_.push_back({ requests[index].arrival_ns, { index } });
        return;
    }
    std::map<std::string, std::size_t> identities;
    for (std::size_t index = 0; index < requests.size(); ++index) identities.emplace(requests[index].id, index);
    std::set<std::size_t> received;
    for (const auto & record : ingress->get_ref<const nlohmann::json::array_t &>()) {
        const auto time_ns = read_parameters({ { "time_ns", record.at("time_ns") } }).at("time_ns");
        if (time_ns < 0 || (!rounds_.empty() && time_ns < rounds_.back().time_ns))
            throw std::invalid_argument("ingress rounds require nonnegative, nondecreasing nanoseconds");
        Round round{ time_ns, {} };
        for (const auto & name : record.at("requests").get_ref<const nlohmann::json::array_t &>()) {
            const auto found = identities.find(name.get<std::string>());
            if (found == identities.end()) throw std::invalid_argument("ingress contains an unknown request: " + name.dump());
            const auto index = found->second;
            if (!received.insert(index).second) throw std::invalid_argument("ingress repeats request: " + requests[index].id);
            if (time_ns < requests[index].arrival_ns)
                throw std::invalid_argument("ingress precedes declared arrival: " + requests[index].id);
            round.requests.push_back(index);
        }
        if (round.requests.empty()) throw std::invalid_argument("ingress round must contain requests");
        rounds_.push_back(std::move(round));
    }
    if (received.size() != requests.size()) throw std::invalid_argument("ingress must deliver every workload request exactly once");
}

std::optional<std::int64_t> RequestDelivery::next_ns() const {
    return next_ == rounds_.size() ? std::nullopt : std::optional{ rounds_[next_].time_ns };
}

bool RequestDelivery::ready(std::int64_t now_ns) const {
    const auto next = next_ns();
    return next && *next <= now_ns;
}

std::vector<std::size_t> RequestDelivery::take(std::int64_t now_ns) {
    std::vector<std::size_t> result;
    while (ready(now_ns)) {
        const auto & requests = rounds_[next_++].requests;
        result.insert(result.end(), requests.begin(), requests.end());
        delivered_.insert(requests.begin(), requests.end());

        // Declared arrivals accumulate while busy; an observed poll retains its actual receive cohort.
        if (observed_) break;
    }
    return result;
}

const std::set<std::size_t> & RequestDelivery::delivered() const { return delivered_; }

} // namespace markov
