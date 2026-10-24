#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/contracts/serving.hpp"
#include "markov/execution/durations.hpp"

namespace markov {

struct ExecutionEvent {
    enum class Kind { Published, Completed };

    std::int64_t time_ns;
    TaskIdentity task;
    Kind kind;
};

// Dependencies make work ready; a resource is acquired only when that work actually starts.
class Executor {
public:
    explicit Executor(const NodeDurations & durations);

    void submit(const RegionGraph & graph, const TaskIdentity & task, const BatchProgram & program);
    std::optional<ExecutionEvent> advance_until(std::optional<std::int64_t> deadline_ns);
    std::int64_t idle_until(const RegionGraph & graph, std::span<const std::size_t> completions, std::int64_t time_ns);
    void retain(std::span<const std::size_t> frontiers);
    std::int64_t now_ns() const;
    nlohmann::json timeline() const;

private:
    struct Notification {
        TaskIdentity task;
        ExecutionEvent::Kind kind;
    };

    struct Work {
        std::size_t resource;
        std::int64_t duration_ns;
        std::size_t remaining = 0;
        std::optional<std::int64_t> started_ns;
        bool completed = false;
        std::vector<std::size_t> successors;
        std::vector<Notification> notifications;
    };

    struct Resource {
        std::optional<std::size_t> running;
        std::set<std::pair<std::int64_t, std::size_t>> ready;
    };

    struct Task {
        std::size_t order;
        std::size_t remaining;
        bool expects_publication;
        bool published = false;
    };

    void ingest(const RegionGraph & graph);
    void ready(std::size_t node);
    void start_ready();
    void complete_work();
    bool completed(std::size_t node) const;
    void watch(std::size_t node, Notification notification);
    void notify(const Notification & notification);

    const NodeDurations & durations_;
    std::deque<Work> work_;
    std::set<std::size_t> completed_frontiers_;
    std::map<std::size_t, Resource> resources_;
    std::set<std::pair<std::int64_t, std::size_t>> completions_;
    std::map<TaskIdentity, Task> tasks_;

    // Same-time task feedback follows submission order, with publication before its own completion.
    std::map<std::tuple<std::int64_t, std::size_t, ExecutionEvent::Kind>, ExecutionEvent> feedback_;
    std::size_t first_node_ = 0;
    std::size_t consumed_edges_ = 0;
    std::size_t next_submission_ = 0;
    std::int64_t now_ns_ = 0;
};

} // namespace markov
