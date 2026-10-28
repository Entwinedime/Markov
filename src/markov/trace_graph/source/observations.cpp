#include "markov/trace_graph/source/observations.hpp"

#include <stdexcept>
#include <utility>

namespace markov::trace_graph::source {

Index Observations::name(const std::string & value) {
    const auto [found, inserted] = name_ids_.try_emplace(value, names.size());
    if (inserted) names.push_back(value);
    return found->second;
}

Index Observations::resource(ResourceKind kind, const std::string & identity) {
    const auto [found, inserted] = resource_ids_.try_emplace(identity, resources.size());
    if (inserted) resources.push_back({ kind, identity });
    return found->second;
}

Index Observations::point(ObservedPoint value) {
    points.push_back(std::move(value));
    return points.size() - 1;
}

void Observations::span(ObservedSpan value) {
    if (value.end_ns < value.begin_ns) throw std::runtime_error("Observed span ends before it starts");
    if (value.end_ns == value.begin_ns) {
        point({ value.begin_ns, value.resource, value.name, value.record });
        return;
    }

    spans.push_back(std::move(value));
}

void Observations::alias_resource(Index from, Index to) { resource_aliases_[from] = to; }

Index Observations::canonical_resource(Index resource) const {
    const auto alias = resource_aliases_.find(resource);
    return alias == resource_aliases_.end() ? resource : alias->second;
}

void Observations::normalize_resources() {
    for (auto & span : spans) span.resource = canonical_resource(span.resource);
    for (auto & point : points) point.resource = canonical_resource(point.resource);
    for (auto & work : work_observations) work.resource = canonical_resource(work.resource);
    for (auto & call : collectives) call.resource = canonical_resource(call.resource);
    for (auto & frame : compute_frames)
        for (auto & call : frame.metadata) call.region.resource = canonical_resource(call.region.resource);
}

Index Observations::describe_work(std::vector<planning::Work> work, std::vector<planning::ComputePart> recipe) {
    const auto [entry, inserted] = work_ids_.try_emplace(std::pair(recipe, work), work_descriptions.size());
    if (inserted) work_descriptions.push_back({ std::move(work), std::move(recipe) });
    return entry->second;
}

void Observations::observe_tokens(const std::string & request, TokenRole role, planning::PrefixId path, RecordRef record) {
    auto & observed = request_tokens[request];
    auto & destination = role == TokenRole::LookupInput ? observed.lookup_input : role == TokenRole::Prompt ? observed.prompt : observed.committed;
    if (destination && destination->path != path) throw std::runtime_error("Source ranks disagree about request token ancestry: " + request);
    if (!destination) destination = TokenObservation{ path, record };
}

void Observations::resolve_flows() {
    struct FlowPair {
        const ObservedFlow * start = nullptr;
        std::size_t starts = 0;
        std::vector<Index> ends;
    };
    std::unordered_map<std::string, FlowPair> pairs;
    pairs.reserve(flows.size() / 2);
    for (const auto & flow : flows) {
        auto & pair = pairs[flow.identity];
        if (flow.start) {
            ++pair.starts;
            pair.start = &flow;
        }
        else pair.ends.push_back(flow.point);
    }
    for (const auto & [identity, pair] : pairs) {
        if (pair.starts != 1 || pair.ends.empty()) {
            const auto reason = pair.starts > 1 ? "ambiguous_flows" : "unpaired_flows";
            constexpr std::size_t diagnostic_example_limit = 5;
            if (++flow_audit[reason] <= diagnostic_example_limit) unresolved_flows.push_back({ identity, pair.starts, pair.ends.size() });
            continue;
        }

        // One producer may cause several device records. Multiple producer
        // occurrences require native disambiguation before reaching this layer.
        if (pair.ends.size() > 1) ++flow_audit["fanout_flows"];
        for (const auto end : pair.ends) {
            if (points[pair.start->point].time_ns > points[end].time_ns) ++flow_audit["flow_clock_order_violations"];
            dependencies.push_back({ pair.start->point, end, pair.start->kind });
            ++flow_audit["paired_flows"];
        }
    }

    // The resolved links own the relationship now; targets do not need raw IDs.
    flows.clear();
    flows.shrink_to_fit();
}

void Observations::release_intake_indexes() {
    name_ids_.clear();
    name_ids_.rehash(0);
    resource_ids_.clear();
    resource_ids_.rehash(0);
    resource_aliases_.clear();
    resource_aliases_.rehash(0);
    work_ids_.clear();
}

} // namespace markov::trace_graph::source
