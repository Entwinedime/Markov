#include "markov/trace_graph/cost/queries.hpp"
#include "markov/trace_graph/source/context.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::cost {

Queries::Queries(const Models & models, const source::SourceContext * source, const source::CpuCosts * cpu_costs)
    : models_(models),
      source_(source),
      source_responses_(source ? source->observations().work_observations.size() : 0) {
    if (source) source_cpu_gaps_ = source::cpu_intervals(source->observations(), source->graph(), source->cpu_gaps(), cpu_costs);
    source_cpu_responses_.resize(source_cpu_gaps_.size());
    if (source) {
        std::vector<source::ObservedRegion> regions;
        std::vector<std::pair<std::size_t, std::size_t>> groups;
        native_cpu_indices_.resize(source->native_work().size());
        for (std::size_t id = 0; id < source->native_work().size(); ++id) {
            const auto & work = source->native_work()[id];
            if (work.timing != source::NativeTiming::Cpu) continue;
            native_cpu_indices_[id] = groups.size();
            const auto first = regions.size();
            auto cursor = work.region.begin_ns;
            for (const auto & excluded : work.excluded) {
                if (cursor < excluded.begin_ns) regions.push_back({ work.region.resource, cursor, excluded.begin_ns, work.region.record });
                cursor = excluded.end_ns;
            }
            if (cursor < work.region.end_ns) regions.push_back({ work.region.resource, cursor, work.region.end_ns, work.region.record });
            groups.emplace_back(first, regions.size());
        }
        const auto measured = source::cpu_intervals(source->observations(), source->graph(), regions, cpu_costs);
        for (const auto & [first, end] : groups) {
            auto & combined = native_cpu_intervals_.emplace_back();
            for (auto id = first; id < end; ++id) combined.add(measured[id]);
        }
        source_native_work_responses_.resize(source->native_work().size());
    }
}

const Response & Queries::query_source_work(const planning::Work & work, std::size_t interval, std::size_t resource) {
    if (!source_) throw std::logic_error("Source work requested without source evidence");
    const auto & original = source_->native_work()[interval];
    if (original.timing != source::NativeTiming::Cpu) {
        if (original.work != work || original.region.resource != resource) return query(work);
        ++requests_;
        auto & response = source_native_work_responses_[interval][0];
        if (!response) {
            const bool dispatch = original.timing == source::NativeTiming::ReadyDispatch;
            const bool service = original.timing == source::NativeTiming::DeviceService;
            response = Response{ Status::Ready,
                                 original.region.end_ns - original.region.begin_ns,
                                 dispatch  ? Coverage::SourceDispatchResidual
                                 : service ? Coverage::SourceService
                                           : Coverage::SourceControlEstimate,
                                 {},
                                 std::nullopt };
            if (dispatch) ++source_dispatch_evaluations_;
            else if (service) ++source_evaluations_;
            else ++source_control_evaluations_;
        }
        return *response;
    }
    auto expected = original.work;
    const bool residual = work.kind.timing_scope != expected.kind.timing_scope;
    if (residual)
        expected.kind.timing_scope = expected.kind.timing_scope == "cpu_service"
                                         ? "residual_interval"
                                         : expected.kind.timing_scope.substr(0, expected.kind.timing_scope.size() - 8) + "_residual_interval";
    if (expected != work || original.region.resource != resource) return query(work);

    const auto & observed = native_cpu_intervals_[native_cpu_indices_[interval]];
    const auto generated_wait = original.generated_wait == source::HostWait::Stream        ? observed.stream_wait_ns
                                : original.generated_wait == source::HostWait::ResultEvent ? observed.event_wait_ns
                                                                                           : 0;
    if (observed.unobserved_ns || observed.wait_ns != generated_wait) return query(work);

    ++requests_;
    auto & response = source_native_work_responses_[interval][residual ? 1 : 0];
    if (!response) {
        const auto raw = residual ? observed.residual_ns : observed.service_ns;
        auto correction = residual ? observed.residual_correction : observed.service_correction;
        correction.add({ 0, raw - correction.covered_ns });
        response = Response{ Status::Ready, correction.cost_ns, Coverage::SourceCpuEstimate, {}, std::nullopt };
        ++source_cpu_work_evaluations_;
    }
    return *response;
}

const Response & Queries::query_source_cpu_gap(std::size_t interval, std::size_t resource) {
    if (!source_ || source_->cpu_gaps()[interval].resource != resource) throw std::logic_error("Source CPU gap requires its original caller");
    ++requests_;
    auto & response = source_cpu_responses_[interval];
    if (response) return *response;
    ++source_cpu_evaluations_;
    const auto & observed = source_cpu_gaps_[interval];
    if (observed.unobserved_ns) response = Response{ Status::UnobservedSource, std::nullopt, std::nullopt, {}, std::nullopt };
    else if (source_->cpu_gap_has_unmodeled_work(interval)) response = Response{ Status::UnmodeledSourceWork, std::nullopt, std::nullopt, {}, std::nullopt };
    else if (observed.wait_ns) response = Response{ Status::UnresolvedSourceWait, std::nullopt, std::nullopt, {}, std::nullopt };
    else {
        auto cost = observed.service_correction;
        cost.add(observed.residual_correction);
        const auto uncorrected = observed.service_ns + observed.residual_ns - cost.covered_ns;
        cost.add({ 0, uncorrected });
        response = Response{ Status::Ready, cost.cost_ns, Coverage::SourceCpuResidual, {}, std::nullopt };
    }
    return *response;
}

const Response & Queries::query_source(const planning::Work & work, std::size_t observation, std::size_t resource) {
    if (!source_) throw std::logic_error("Source service requested without source evidence");
    const auto & observed = source_->observations();
    const auto & description = observed.work_descriptions[observed.work_observations[observation].description];
    const auto & service = source_->device_service(observation);
    if (description.observed.size() != 1 || description.observed.front() != work) throw std::logic_error("Source service does not match the target work");

    // A matching work description is insufficient when native ownership is
    // ambiguous. Such work still requires an independent model.
    if (!service.available()) return query(work);
    if (service.resource != resource) throw std::logic_error("Source service does not match the target resource");

    ++requests_;
    auto & response = source_responses_[observation];
    if (!response) {
        graph::Time duration = 0;
        for (const auto id : service.nodes) {
            const auto value = source_->graph().graph.nodes()[id].duration_ns;
            if (value > std::numeric_limits<graph::Time>::max() - duration) throw std::overflow_error("Source device service exceeds the clock range");
            duration += value;
        }
        response = Response{ Status::Ready, duration, Coverage::SourceService, {}, std::nullopt };
        ++source_evaluations_;
    }
    return *response;
}

const Response & Queries::query(const planning::Work & work) {
    ++requests_;
    const auto found = responses_.find(work);
    if (found != responses_.end()) return found->second;

    const auto & response = responses_.emplace(work, models_.query(work)).first->second;
    if (response.model && !models_.model(*response.model).limitations.empty()) limited_models_.insert(*response.model);
    return response;
}

const Response & Queries::query_source_delay(graph::NodeId id, std::size_t resource) {
    if (!source_) throw std::logic_error("Source delay requested without source evidence");
    const auto & node = source_->graph().graph.nodes()[id];
    if (node.resource != resource || node.kind != graph::NodeKind::ResidualGap
        || source_->observations().resources[resource].kind != source::ResourceKind::Client)
        throw std::logic_error("Source delay is not a retained client interval on the target resource");

    ++requests_;
    return source_delays_.try_emplace(id, Response{ Status::Ready, node.duration_ns, Coverage::SourceResidual, {}, std::nullopt }).first->second;
}

} // namespace markov::trace_graph::cost
