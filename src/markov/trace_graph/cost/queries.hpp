#pragma once

#include "markov/trace_graph/cost/models.hpp"
#include "markov/trace_graph/planning/work.hpp"
#include "markov/trace_graph/source/services.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace markov::trace_graph::source {
class SourceContext;
}

namespace markov::trace_graph::cost {

// Models and optional source evidence outlive this immutable-input session.
// Independent responses are keyed by work; source services keep their identity.
class Queries {
public:
    explicit Queries(const Models & models, const source::SourceContext * source = nullptr, const source::CpuCosts * cpu_costs = nullptr);
    const Response & query(const planning::Work & work);
    const Response & query_source(const planning::Work & work, std::size_t observation, std::size_t resource);
    const Response & query_source_delay(graph::NodeId node, std::size_t resource);
    const Response & query_source_cpu_gap(std::size_t interval, std::size_t resource);
    const Response & query_source_cpu_work(const planning::Work & work, std::size_t interval, std::size_t resource);
    std::size_t requests() const { return requests_; }
    std::size_t evaluations() const {
        return responses_.size() + source_evaluations_ + source_delays_.size() + source_cpu_evaluations_ + source_cpu_work_evaluations_;
    }
    std::size_t source_evaluations() const { return source_evaluations_; }
    std::size_t residual_evaluations() const { return source_delays_.size(); }
    std::size_t cpu_residual_evaluations() const { return source_cpu_evaluations_; }
    std::span<const source::CpuInterval> source_cpu_gaps() const { return source_cpu_gaps_; }
    std::span<const source::CpuInterval> source_cpu_work() const { return source_cpu_work_; }
    std::size_t cpu_work_evaluations() const { return source_cpu_work_evaluations_; }
    const std::set<std::size_t> & limited_models() const { return limited_models_; }

private:
    const Models & models_;
    const source::SourceContext * source_;
    std::map<planning::Work, Response> responses_;
    std::set<std::size_t> limited_models_;
    std::vector<std::optional<Response>> source_responses_;
    std::map<graph::NodeId, Response> source_delays_;
    std::vector<source::CpuInterval> source_cpu_gaps_;
    std::vector<std::optional<Response>> source_cpu_responses_;
    std::vector<source::CpuInterval> source_cpu_work_;
    std::vector<std::array<std::optional<Response>, 2>> source_cpu_work_responses_;
    std::size_t source_cpu_work_evaluations_ = 0;
    std::size_t source_cpu_evaluations_ = 0;
    std::size_t source_evaluations_ = 0;
    std::size_t requests_ = 0;
};

} // namespace markov::trace_graph::cost
