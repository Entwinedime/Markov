#include "markov/trace_graph/cost/queries.hpp"

namespace markov::trace_graph::cost {

const Response & Queries::query(const planning::Work & work) {
    ++requests_;
    const auto found = responses_.find(work);
    if (found != responses_.end()) return found->second;

    return responses_.emplace(work, models_.query(work)).first->second;
}

} // namespace markov::trace_graph::cost
