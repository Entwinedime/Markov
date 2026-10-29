#include "markov/commands/costs.hpp"

#include "markov/adapters/source.hpp"
#include "markov/trace_graph/source/services.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace markov::commands {
namespace {

using namespace trace_graph::source;
using Json = nlohmann::json;

struct Region {
    ObservedRegion source;
    std::size_t role;
};

struct Summary {
    std::size_t regions = 0;
    CpuInterval observed;

    void add(const CpuInterval & value) {
        ++regions;
        observed.add(value);
    }

    Json report() const {
        const auto estimate = [](Time raw, CpuCorrection correction) -> Json {
            return {
                {          "observed_ns",                         raw },
                {           "covered_ns",       correction.covered_ns },
                { "estimated_covered_ns",          correction.cost_ns },
                {       "uncorrected_ns", raw - correction.covered_ns }
            };
        };
        return {
            { "regions", regions },
            { "service", estimate(observed.service_ns, observed.service_correction) },
            { "residual", estimate(observed.residual_ns, observed.residual_correction) },
            { "wait_ns", observed.wait_ns },
            { "instrumentation_ns", observed.instrumentation_ns },
            { "unobserved_ns", observed.unobserved_ns },
            { "excluded_correction", { { "covered_ns", observed.excluded_correction.covered_ns }, { "cost_ns", observed.excluded_correction.cost_ns } } }
        };
    }
};

} // namespace

Json cpu_cost_summary(std::span<const CpuInterval> intervals) {
    Summary summary;
    for (const auto & interval : intervals) summary.add(interval);
    return summary.report();
}

Json source_cpu_cost_report(const adapters::PreparedSource & prepared) {
    if (!prepared.cpu_costs) return nullptr;

    const auto & context = prepared.context;
    const auto & observed = context.observations();
    const auto & costs = *prepared.cpu_costs;
    std::vector<std::string> roles{ "compute_calls", "collective_envelopes", "outside_known_calls" };
    std::vector<ObservedRegion> regions;
    std::vector<std::size_t> owners;
    std::vector<bool> formal;
    auto requests = Json::array();
    for (const auto & request : observed.requests) {
        const auto first = regions.size();
        std::map<Index, std::vector<Region>> lanes;
        for (const auto frame : context.request_frames(request.identity)) {
            for (const auto id : context.frame_operations(frame)) {
                const auto & call = observed.work_observations[id];
                lanes[call.resource].push_back({
                    { call.resource, call.begin_ns, call.end_ns, call.record },
                    0
                });
            }
            for (const auto & call : observed.compute_frames[frame].preparations) {
                const auto role = static_cast<std::size_t>(std::ranges::find(roles, call.role) - roles.begin());
                if (role == roles.size()) roles.push_back(call.role);
                lanes[call.region.resource].push_back({ call.region, role });
            }
            for (const auto id : context.frame_collectives(frame)) {
                const auto & call = observed.collectives[id];
                lanes[call.resource].push_back({
                    { call.resource, call.begin_ns, call.end_ns, call.record },
                    1
                });
            }
        }

        // Partition each caller inside HTTP boundaries. Known calls and their
        // complement are exclusive; worker/device and other threads are not added.
        for (auto & [resource, calls] : lanes) {
            std::ranges::sort(calls, {}, [](const Region & region) { return region.source.begin_ns; });
            auto cursor = request.begin_ns;
            for (const auto & call : calls) {
                const auto begin = std::max(request.begin_ns, call.source.begin_ns), end = std::min(request.end_ns, call.source.end_ns);
                if (end <= begin) continue;
                if (begin < cursor) throw std::runtime_error("CPU call ownership overlaps inside request: " + request.identity);
                if (cursor < begin) {
                    regions.push_back({ resource, cursor, begin, call.source.record });
                    owners.push_back(2);
                }
                regions.push_back({ resource, begin, end, call.source.record });
                owners.push_back(call.role);
                cursor = end;
            }
            if (cursor < request.end_ns) {
                regions.push_back({ resource, cursor, request.end_ns, {} });
                owners.push_back(2);
            }
        }
        formal.insert(formal.end(), regions.size() - first, request.formal);
        requests.push_back({
            {          "request",                  request.identity },
            {           "formal",                    request.formal },
            {   "caller_threads",                      lanes.size() },
            { "observed_http_ns", request.end_ns - request.begin_ns }
        });
    }

    const auto measured = cpu_intervals(observed, context.graph(), regions, &costs.intervals);
    std::vector<Summary> summaries(roles.size()), formal_summaries(roles.size());
    for (std::size_t i = 0; i < measured.size(); ++i) {
        summaries[owners[i]].add(measured[i]);
        if (formal[i]) formal_summaries[owners[i]].add(measured[i]);
    }
    auto partition = Json::object(), formal_partition = Json::object();
    for (std::size_t i = 0; i < roles.size(); ++i) {
        partition[roles[i]] = summaries[i].report();
        formal_partition[roles[i]] = formal_summaries[i].report();
    }
    return {
        {                   "input",costs.path.string()                                    },
        {                   "basis",                        costs.basis },
        {                 "sources",                      costs.sources },
        {        "correction_spans",            costs.intervals.spans() },
        {        "caller_partition",               std::move(partition) },
        { "formal_caller_partition",        std::move(formal_partition) },
        {                "requests",                std::move(requests) },
        { "original_graph_modified",                              false },
        {           "qualification",
         "Source-coordinate estimates only. Known caller envelopes and their complement partition each HTTP window; worker, device "
         "and other threads are outside this summary. Corrections intersect observed service and residual separately. Wait/identified-overhead "
         "overlaps are excluded. Uncovered time remains uncorrected; unrecognized waits can still reside in observed service. Proportional budgets "
         "do not prove per-operation service or transfer eligibility." }
    };
}

} // namespace markov::commands
