#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <span>
#include <vector>

#include "markov/contracts/serving.hpp"
#include "markov/execution/durations.hpp"

namespace markov {

struct ExecutionEvent {
    enum class Kind { Published, Completed };

    Kind kind;
    std::int64_t time_ns;
};

// Schedule newly appended work; prior resource frontiers retain their computed finish times.
class Executor {
public:
    explicit Executor(const NodeDurations & durations);

    void submit(const RegionGraph & graph, const BatchProgram & program);
    std::int64_t evaluate(const RegionGraph & graph, std::span<const std::size_t> completions);
    void retain(std::span<const std::size_t> frontiers);
    bool has_events() const;
    ExecutionEvent next_event();
    void advance_to(std::int64_t time_ns);
    std::int64_t now_ns() const;

private:
    std::int64_t finish_time(std::size_t node) const;

    const NodeDurations & durations_;
    std::vector<std::int64_t> finished_ns_;
    std::map<std::size_t, std::int64_t> frontier_ns_;
    std::size_t first_node_ = 0;
    std::size_t consumed_edges_ = 0;
    std::int64_t now_ns_ = 0;
    std::deque<ExecutionEvent> events_;
};

} // namespace markov
