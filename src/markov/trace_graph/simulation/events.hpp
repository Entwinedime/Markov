#pragma once

#include <cstddef>
#include <cstdint>

namespace markov::trace_graph::simulation {

using Component = std::uint32_t;

struct Action {
    std::uint32_t kind;
    std::size_t subject;
};

enum class EventOrigin { Completion, Timer };

struct Event {
    Component owner;
    Action action;
    EventOrigin origin;
    std::size_t identity;
};

enum class OperationState : std::uint8_t { Pending, Ready, Queued, Running, Complete, Cancelled };

} // namespace markov::trace_graph::simulation
