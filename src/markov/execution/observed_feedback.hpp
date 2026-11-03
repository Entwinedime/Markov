#pragma once

#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "markov/execution/feedback.hpp"

namespace markov {

// Replay completion times, never control decisions or graph work.
class ObservedFeedback final : public ExecutionFeedback {
public:
    ObservedFeedback(const nlohmann::json & observation, std::span<const Request> requests);

    void submit(const RegionGraph & graph, const ServingAction & action, const BatchProgram & program) override;
    std::optional<ExecutionEvent> advance_until(std::optional<std::int64_t> deadline_ns) override;
    void retain(std::span<const std::size_t> frontiers) override;
    std::int64_t now_ns() const override;
    nlohmann::json finish(bool details) const override;

private:
    using EventOrder = std::pair<std::int64_t, std::size_t>;
    struct Record {
        nlohmann::json work;
        bool publishes;
        nlohmann::json observations;
        std::optional<TaskIdentity> task{};
    };
    struct Event {
        std::size_t record;
        ExecutionEvent::Kind kind;
    };

    nlohmann::json identity(const ServingAction & action) const;
    EventOrder add_event(const nlohmann::json & value, std::size_t record, ExecutionEvent::Kind kind);

    std::string source_;
    std::vector<std::string> requests_;
    std::vector<Record> records_;
    std::map<nlohmann::json, std::deque<std::size_t>> remaining_;
    std::map<EventOrder, Event> events_;
    std::int64_t now_ns_ = 0;
};

} // namespace markov
