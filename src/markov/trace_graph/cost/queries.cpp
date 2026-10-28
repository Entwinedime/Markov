#include "markov/trace_graph/cost/queries.hpp"
#include "markov/trace_graph/source/context.hpp"

#include <limits>
#include <stdexcept>

namespace markov::trace_graph::cost {

Queries::Queries(const Models & models, const source::SourceContext * source)
    : models_(models),
      source_(source),
      source_responses_(source ? source->observations().work_observations.size() : 0) {}

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

    return responses_.emplace(work, models_.query(work)).first->second;
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
