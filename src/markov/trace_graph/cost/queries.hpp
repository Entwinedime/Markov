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
#include <utility>
#include <vector>

namespace markov::trace_graph::source {
class SourceContext;
}

namespace markov::trace_graph::cost {

// Models, optional source evidence and corrections outlive this session.
// Independent responses are keyed by work; source services keep their identity.
class Queries {
public:
    explicit Queries(const Models & models, const source::SourceContext * source = nullptr, const source::CpuCosts * cpu_costs = nullptr);
    const Response & query(const planning::Work & work);
    const Response & query_source(const planning::Work & work, std::size_t observation, std::size_t resource);
    const Response & query_region_gap(std::size_t interval);
    const Response & query_client_delay(std::size_t request, bool response);
    const Response & query_source_cpu_gap(std::size_t interval, std::size_t resource);
    const Response & query_source_work(const planning::Work & work, std::size_t interval, std::size_t resource);
    std::size_t requests() const { return requests_; }
    std::size_t evaluations() const {
        return responses_.size() + source_evaluations_ + region_gaps_.size() + client_delays_.size() + source_cpu_evaluations_ + source_cpu_work_evaluations_
               + source_dispatch_evaluations_ + source_control_evaluations_;
    }
    std::size_t source_evaluations() const { return source_evaluations_; }
    std::size_t residual_evaluations() const { return region_gaps_.size() + client_delays_.size(); }
    std::size_t cpu_residual_evaluations() const { return source_cpu_evaluations_; }
    std::span<const source::CpuInterval> source_cpu_gaps();
    std::span<const source::CpuInterval> native_cpu_intervals();
    std::size_t cpu_work_evaluations() const { return source_cpu_work_evaluations_; }
    std::size_t dispatch_evaluations() const { return source_dispatch_evaluations_; }
    std::size_t control_evaluations() const { return source_control_evaluations_; }
    const std::set<std::size_t> & limited_models() const { return limited_models_; }

private:
    void prepare_source_costs();

    const Models & models_;
    const source::SourceContext * source_;
    const source::CpuCosts * cpu_costs_;
    bool source_costs_prepared_ = false;
    std::map<planning::Work, Response> responses_;
    std::set<std::size_t> limited_models_;
    std::vector<std::optional<Response>> source_responses_;
    std::map<std::size_t, Response> region_gaps_;
    std::map<std::pair<std::size_t, bool>, Response> client_delays_;
    std::vector<source::CpuInterval> source_cpu_gaps_;
    std::vector<std::optional<Response>> source_cpu_responses_;
    std::vector<source::CpuInterval> native_cpu_intervals_;
    std::vector<std::size_t> native_cpu_indices_;
    std::vector<std::array<std::optional<Response>, 2>> source_native_work_responses_;
    std::size_t source_cpu_work_evaluations_ = 0;
    std::size_t source_dispatch_evaluations_ = 0, source_control_evaluations_ = 0;
    std::size_t source_cpu_evaluations_ = 0;
    std::size_t source_evaluations_ = 0;
    std::size_t requests_ = 0;
};

} // namespace markov::trace_graph::cost
