#pragma once

#include "markov/trace_graph/cost/models.hpp"
#include "markov/trace_graph/planning/work.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <vector>

namespace markov::trace_graph::source {
class SourceContext;
}

namespace markov::trace_graph::cost {

// Models and optional source evidence outlive this immutable-input session.
// Independent responses are keyed by work; source services keep their identity.
class Queries {
public:
    explicit Queries(const Models & models, const source::SourceContext * source = nullptr);
    const Response & query(const planning::Work & work);
    const Response & query_source(const planning::Work & work, std::size_t observation, std::size_t resource);
    const Response & query_source_delay(graph::NodeId node, std::size_t resource);
    std::size_t requests() const { return requests_; }
    std::size_t evaluations() const { return responses_.size() + source_evaluations_ + source_delays_.size(); }
    std::size_t source_evaluations() const { return source_evaluations_; }
    std::size_t residual_evaluations() const { return source_delays_.size(); }

private:
    const Models & models_;
    const source::SourceContext * source_;
    std::map<planning::Work, Response> responses_;
    std::vector<std::optional<Response>> source_responses_;
    std::map<graph::NodeId, Response> source_delays_;
    std::size_t source_evaluations_ = 0;
    std::size_t requests_ = 0;
};

} // namespace markov::trace_graph::cost
