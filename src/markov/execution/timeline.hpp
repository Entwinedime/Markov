#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "markov/execution/observer.hpp"
#include "markov/execution/timing.hpp"
#include "markov/graph/graph.hpp"

namespace markov {

struct EventCompletion {
    std::size_t event;
    std::int64_t time_ns;
};

// An input connects an existing execution event to an event in the new invocation.
struct ExecutionInput {
    std::size_t source;
    std::size_t target;
    Relation relation;
};

class Timeline {
public:
    explicit Timeline(std::vector<Resource> resources, ExecutionObserver * observer = nullptr);

    // Accept a validated graph and bound input readiness; check execution resources, durations and boundary times here.
    std::vector<std::size_t> insert(const CoarseGraph & graph, std::span<const ServiceDuration> work_ns, std::span<const ExecutionInput> inputs,
                                    std::span<const BoundaryTime> boundaries = {});
    std::optional<EventCompletion> advance(std::optional<std::int64_t> until_ns = {});
    bool completed(std::size_t event) const;
    std::size_t defer();
    void resolve(std::size_t event, std::span<const std::size_t> sources, Relation relation = Relation::Communication,
                 std::optional<TimeWindow> window = {}, ServiceDuration service_ns = 0);
    void retain(std::set<std::size_t> external);
    bool done() const;
    std::int64_t now_ns() const;
    std::size_t work_count() const;

private:
    struct Dependency {
        std::size_t target;
        std::int64_t delay_ns;
    };

    struct Wait {
        std::size_t resource;
        std::size_t finish;
    };

    struct Event {
        std::optional<std::size_t> resource;
        std::size_t pending = 0;
        std::size_t holds = 0;
        std::int64_t ready_ns = 0;
        std::optional<std::int64_t> completed_ns;
        std::optional<std::int64_t> latest_ns;
        bool activated = false;
        std::vector<Dependency> next;
        std::vector<Wait> waits;
        std::vector<std::size_t> enqueues;
    };

    struct Command {
        std::size_t resource;
        std::size_t finish;
        std::size_t pending;
    };

    void connect(std::size_t source, std::size_t target, std::int64_t delay_ns, Relation relation);
    void schedule(std::size_t event);

    std::vector<Resource> resources_;
    std::unordered_map<std::size_t, Event> events_;
    std::unordered_map<std::size_t, Command> commands_;
    std::size_t next_event_ = 0;
    std::size_t next_command_ = 0;
    std::vector<std::optional<std::size_t>> tails_;
    std::vector<std::set<std::size_t>> queued_;
    std::vector<std::optional<std::size_t>> completed_commands_;
    using Ready = std::pair<std::int64_t, std::size_t>;
    std::priority_queue<Ready, std::vector<Ready>, std::greater<>> ready_;
    std::size_t unfinished_ = 0;
    std::size_t work_count_ = 0;
    std::int64_t now_ns_ = 0;
    ExecutionObserver * observer_;
};

} // namespace markov
