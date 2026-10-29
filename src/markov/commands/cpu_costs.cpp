#include "markov/commands/costs.hpp"

#include "markov/adapters/source.hpp"
#include "markov/trace_graph/source/services.hpp"

#include <algorithm>
#include <map>
#include <optional>
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

Json source_result_cpu_report(const adapters::PreparedSource & prepared) {
    const auto & context = prepared.context;
    const auto & observed = context.observations();
    std::vector<ObservedRegion> regions;
    auto batches = Json::array();
    std::map<Index, std::vector<Index>> submissions;
    for (Index id = 0; id < observed.native_queue_calls.size(); ++id) submissions[observed.native_queue_calls[id].submission.resource].push_back(id);
    const auto submitted = [&](Index id) { return observed.native_queue_calls[id].submission.begin_ns; };
    for (auto & [resource, lane] : submissions) std::ranges::sort(lane, {}, submitted);

    for (const auto & batch : observed.batch_results) {
        if (!batch.processed) continue;
        const auto & processed = *batch.processed;
        const auto & parent = processed.region;
        const auto parent_index = regions.size();
        regions.push_back(parent);
        std::vector<Time> boundaries{ parent.begin_ns, parent.end_ns };
        for (const auto id : processed.controls) {
            const auto & control = observed.host_controls[id].region;
            boundaries.insert(boundaries.end(), { control.begin_ns, control.end_ns });
        }
        for (const auto & send : batch.final_sends) boundaries.insert(boundaries.end(), { send.region.begin_ns, send.region.end_ns });
        std::ranges::sort(boundaries);
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

        auto parts = Json::array();
        for (std::size_t i = 1; i < boundaries.size(); ++i) {
            const auto begin = boundaries[i - 1], end = boundaries[i];
            if (begin < parent.begin_ns || parent.end_ns < end) throw std::runtime_error("Result control lies outside its processing interval");
            std::optional<Index> owner;
            Time smallest = parent.end_ns - parent.begin_ns + 1;
            for (const auto id : processed.controls) {
                const auto & control = observed.host_controls[id].region;
                if (control.begin_ns <= begin && end <= control.end_ns && control.end_ns - control.begin_ns < smallest) {
                    owner = id;
                    smallest = control.end_ns - control.begin_ns;
                }
            }
            std::string role = owner ? observed.host_controls[*owner].work.kind.operation : "caller";
            std::string position = batch.final_sends.empty() ? "no_final_send" : "before_send";
            for (const auto & send : batch.final_sends) {
                if (send.region.resource != parent.resource) throw std::runtime_error("Result send changed its processing caller");
                if (send.region.begin_ns <= begin && end <= send.region.end_ns) {
                    if (owner) throw std::runtime_error("Result send overlaps a cache control");
                    role = "final_send";
                    position = "sending";
                }
                else if (send.region.end_ns <= begin) position = "after_send";
            }
            const auto & lane = submissions[parent.resource];
            const auto first = std::ranges::lower_bound(lane, begin, {}, submitted), last = std::ranges::lower_bound(lane, end, {}, submitted);
            auto calls = Json::array();
            for (auto at = first; at != last; ++at)
                calls.push_back({
                    { "source_index",                                                   *at },
                    {         "name", observed.names[observed.native_queue_calls[*at].name] }
                });
            parts.push_back({
                {       "region_index",                       regions.size() },
                {               "role",                                 role },
                {            "control", owner ? Json(*owner) : Json(nullptr) },
                {           "position",                             position },
                {           "begin_ns",                                begin },
                {             "end_ns",                                  end },
                { "native_submissions",                         last - first },
                {       "native_calls",                     std::move(calls) }
            });
            regions.push_back({ parent.resource, begin, end, parent.record });
        }

        // Result processing runs on the scheduler after its latest submitted
        // batch, which can be a different frame from the result being consumed.
        const BatchResult * predecessor = nullptr;
        for (const auto & candidate : observed.batch_results)
            if (candidate.run.resource == parent.resource && candidate.run.end_ns <= parent.begin_ns
                && (!predecessor || predecessor->run.end_ns < candidate.run.end_ns))
                predecessor = &candidate;
        batches.push_back({
            {          "source_frame",                                            batch.frame },
            {              "resource",                                        parent.resource },
            {   "parent_region_index",                                           parent_index },
            { "after_submitted_frame", predecessor ? Json(predecessor->frame) : Json(nullptr) },
            {                 "parts",                                       std::move(parts) }
        });
    }

    const auto measured = cpu_intervals(observed, context.graph(), regions, prepared.cpu_costs ? &prepared.cpu_costs->intervals : nullptr);
    std::map<std::string, Summary> roles;
    for (auto & batch : batches) {
        const auto parent = batch["parent_region_index"].get<Index>();
        batch.erase("parent_region_index");
        batch["parent_cpu"] = cpu_cost_summary(std::span(measured).subspan(parent, 1));
        for (auto & part : batch["parts"]) {
            const auto index = part["region_index"].get<Index>();
            part.erase("region_index");
            part["cpu"] = cpu_cost_summary(std::span(measured).subspan(index, 1));
            roles[part["role"].get<std::string>()].add(measured[index]);
        }
    }
    auto totals = Json::object();
    for (const auto & [role, summary] : roles) totals[role] = summary.report();
    return {
        {                 "batches",                                                 std::move(batches)                                    },
        {         "exclusive_roles",                                                                                      std::move(totals) },
        { "original_graph_modified",                                                                                                  false },
        {           "qualification",
         "Each caller interval is partitioned once; nested controls belong to the innermost observation. Parent CPU totals are "
         "comparison envelopes, not additional charges. Service, waits, residual and identified overhead remain separate. Native "
         "submission counts expose work still requiring a rule. These are source observations, not target costs or full HTTP predictions." }
    };
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
