#pragma once

#include "markov/trace_graph/graph/graph.hpp"
#include "markov/trace_graph/simulation/events.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <queue>
#include <span>
#include <vector>

namespace markov::trace_graph::simulation {

class Execution;

class EventReceiver {
public:
    virtual ~EventReceiver() = default;
    virtual void on_event(Execution & execution, const Event & event) = 0;
};

struct Operation {
    graph::Node node;
    std::optional<Action> completion = std::nullopt;
    std::optional<Component> owner = std::nullopt;
};

struct Mutation {
    Component owner;
    std::vector<Operation> operations;
    std::vector<graph::Edge> dependencies;
};

struct ExecutionResult {
    // Only completed operations have final start/end times. Report unavailable
    // times as null; cancelled storage work is not a zero-duration service.
    std::vector<graph::Time> starts_ns;
    std::vector<graph::Time> completions_ns;
    std::vector<OperationState> states;
    std::size_t executed = 0;
    std::size_t edges = 0;
};

// The same clock schedules observed graphs and generated work. New mutations
// append owned operations atomically; dependencies may only target the new batch.
// Completion actions are routed directly, without polling unrelated components.
class Execution {
public:
    Execution(const graph::Graph & graph, std::span<const graph::Time> initial_availability_ns);
    Execution(std::size_t resources, EventReceiver & receiver);

    graph::NodeId next_node() const { return result_.starts_ns.size(); }
    graph::NodeId submit(Mutation mutation);
    std::size_t schedule_at(graph::Time time_ns, Component owner, Action action);
    void cancel_timer(std::size_t timer);
    bool cancel(Component owner, graph::NodeId node);
    void run();
    void stop() { stopped_ = true; }

    graph::Time now_ns() const { return now_ns_; }
    const ExecutionResult & result() const { return result_; }
    ExecutionResult take_result();

private:
    enum class ScheduledKind { Completion, Available, Timer };
    struct Scheduled {
        graph::Time at;
        ScheduledKind kind;
        std::size_t identity;
        bool operator>(const Scheduled & other) const;
    };
    struct OwnedOperation {
        Operation operation;
        Component owner;
        std::vector<graph::Edge> successors;
    };
    struct Lane {
        std::optional<graph::NodeId> running;
        std::optional<graph::NodeId> previous;
        std::deque<graph::NodeId> ready;
    };
    struct Timer {
        Component owner;
        Action action;
        bool active = true;
    };

    const graph::Node & node(graph::NodeId id) const;
    void ready(graph::NodeId id);
    void available(graph::NodeId id);
    void start(graph::NodeId id);
    void complete(graph::NodeId id);
    void start_next(std::size_t resource);
    void release(const graph::Edge & edge);
    void record_edge(graph::Edge edge);

    const graph::Graph * source_ = nullptr;
    std::size_t original_nodes_ = 0;
    EventReceiver * receiver_ = nullptr;
    graph::Time now_ns_ = 0;
    bool stopped_ = false;
    ExecutionResult result_;
    std::vector<std::size_t> incoming_;
    std::vector<OwnedOperation> owned_;
    std::map<graph::NodeId, std::vector<graph::Edge>> source_additions_;
    std::vector<Lane> lanes_;
    std::vector<Timer> timers_;
    std::priority_queue<Scheduled, std::vector<Scheduled>, std::greater<Scheduled>> scheduled_;
};

} // namespace markov::trace_graph::simulation
