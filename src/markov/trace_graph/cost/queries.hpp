#pragma once

#include "markov/trace_graph/cost/models.hpp"
#include "markov/trace_graph/planning/work.hpp"

#include <cstddef>
#include <map>

namespace markov::trace_graph::cost {

// One immutable selected catalog backs one prediction session. All response
// states are cached by complete work, including gaps; no fitting occurs here.
class Queries {
public:
    explicit Queries(const Models & models) : models_(models) {}
    const Response & query(const planning::Work & work);
    std::size_t requests() const { return requests_; }
    std::size_t evaluations() const { return responses_.size(); }

private:
    const Models & models_;
    std::map<planning::Work, Response> responses_;
    std::size_t requests_ = 0;
};

} // namespace markov::trace_graph::cost
