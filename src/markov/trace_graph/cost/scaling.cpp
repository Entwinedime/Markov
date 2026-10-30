#include "markov/trace_graph/cost/scaling.hpp"

#include <limits>
#include <stdexcept>

namespace markov::trace_graph::cost {

graph::Time NodeScale::apply(std::string_view name, graph::Time duration_ns) {
    for (std::size_t id = 0; id < rules_.size(); ++id) {
        const auto & rule = rules_[id];
        if (!name.contains(rule.name)) continue;

        const long double scaled = duration_ns * static_cast<long double>(rule.factor);
        if (scaled > std::numeric_limits<graph::Time>::max()) throw std::overflow_error("NodeScale duration exceeds nanosecond range");
        ++matched_[id];
        return static_cast<graph::Time>(scaled);
    }
    return duration_ns;
}

} // namespace markov::trace_graph::cost
