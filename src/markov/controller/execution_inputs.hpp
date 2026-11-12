#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>

#include <nlohmann/json.hpp>

#include "markov/contracts/serving.hpp"
#include "markov/execution/timing.hpp"

namespace markov {

class ControlObserver;

struct InputPosition {
    std::string group;
    std::size_t step;
};

struct InvocationInput {
    GraphTiming timing;
    InputPosition position;
};

// Positions retain an already checked causal prefix; operation and work must still match at every step.
class ExecutionInputs {
public:
    ExecutionInputs(const nlohmann::json & source, ControlObserver * observer);
    void bind(std::span<const std::string> participants) const;
    static nlohmann::json work_identity(const nlohmann::json & work);
    static nlohmann::json action_identity(const ServingAction & action);
    static nlohmann::json notification_identity(const nlohmann::json & value);
    InvocationInput graph(const std::string & group, const std::string & operation, const nlohmann::json & identity, const CoarseGraph & graph);
    std::optional<TimeWindow> input(const std::string & group, const nlohmann::json & identity, bool in_flow);
    TimeWindow notification(const nlohmann::json & identity);
    EventTiming transfer(std::span<const InputPosition> posts);
    nlohmann::json finish() const;

private:
    struct Actor {
        nlohmann::json steps;
        std::optional<TimeWindow> unmeasured_completion_window;
        std::size_t next = 0;
    };
    struct Condition {
        nlohmann::json value;
        bool consumed = false;
    };

    const nlohmann::json & take(const std::string & group, const std::string & operation, const nlohmann::json & identity);
    static TimeWindow window(const nlohmann::json & record);
    static ServiceDuration service(const nlohmann::json & record);

    std::map<std::string, Actor> actors_;
    ControlObserver * observer_;
    std::map<nlohmann::json, Condition> notifications_;
    std::map<nlohmann::json, Condition> transfers_;
    std::size_t graph_count_ = 0;
    std::size_t input_count_ = 0;
};

} // namespace markov
