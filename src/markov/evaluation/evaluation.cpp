#include "markov/evaluation/evaluation.hpp"
#include "markov/commands/costs.hpp"
#include "markov/commands/trajectory.hpp"
#include "markov/trace_graph/source/regions.hpp"

#include <compare>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace markov::evaluation {
namespace {

using namespace trace_graph;
using Json = nlohmann::json;

Json read(const std::filesystem::path & path) {
    std::ifstream stream(path);
    return Json::parse(stream);
}

struct Address {
    std::optional<std::size_t> request;
    std::size_t iteration;
    std::optional<std::size_t> participant;
    std::string port;
    std::optional<planning::Work> work;
    std::optional<std::vector<std::int64_t>> data = std::nullopt;
    auto operator<=>(const Address &) const = default;
};

struct Measurement {
    graph::Time duration_ns;
    const source::ObservedCost * observation;
};

std::optional<Address> address(const Json & node) {
    if (node.at("origin").at("type") == "source_client") {
        const auto & origin = node.at("origin");
        return Address{ origin.at("request").get<std::size_t>(), 0, std::nullopt, origin.at("response").get<bool>() ? "egress" : "ingress", std::nullopt };
    }
    if (!node.contains("position")) return std::nullopt;

    const auto & position = node.at("position");
    auto participant = position.at("participant").is_null() ? std::nullopt : std::optional(position.at("participant").get<std::size_t>());
    std::optional<planning::Work> work;
    if (!node.at("work").is_null()) {
        const auto & raw = node.at("work");
        work = planning::Work{
            { raw.at("operation").get<std::string>(), raw.at("implementation").get<std::string>(), raw.at("timing_scope").get<std::string>() },
            raw.at("features").get<planning::Features>()
        };
    }
    return Address{ position.at("request").is_null() ? std::nullopt : std::optional(position.at("request").get<std::size_t>()),
                    position.at("iteration").get<std::size_t>(),
                    participant,
                    position.at("port").get<std::string>(),
                    std::move(work),
                    position.contains("data") ? std::optional(position.at("data").get<std::vector<std::int64_t>>()) : std::nullopt };
}

Json order_report(const Json & serialized, const std::vector<std::pair<const source::ObservedCost *, std::size_t>> & matched) {
    using Interval = std::pair<trace_graph::graph::Time, trace_graph::graph::Time>;
    std::map<std::size_t, std::map<Interval, std::vector<std::size_t>>> lanes;
    for (const auto & [observation, node] : matched) {
        const auto & scope = observation->work.kind.timing_scope;
        if (!observation->contiguous || (scope != "thread_cpu" && scope != "synchronous_phase_interval" && scope != "storage_worker_service")) continue;
        const auto & region = observation->region;
        lanes[region.resource][{ region.begin_ns, region.end_ns }].push_back(node);
    }
    std::vector<std::vector<std::size_t>> successors(serialized.at("nodes").size());
    for (const auto & edge : serialized.at("edges")) successors[edge.at(0).get<std::size_t>()].push_back(edge.at(1).get<std::size_t>());
    std::map<std::size_t, std::vector<bool>> reachable;
    auto violations = Json::array();
    std::size_t checked = 0, overlapping_groups = 0;
    for (const auto & [resource, intervals] : lanes) {
        auto previous = intervals.end();
        for (auto current = intervals.begin(); current != intervals.end(); ++current) {
            if (previous != intervals.end() && previous->first.second <= current->first.first) {
                for (const auto before : previous->second) {
                    auto [known, inserted] = reachable.try_emplace(before);
                    if (inserted) {
                        known->second.resize(successors.size(), false);
                        std::vector<std::size_t> pending{ before };
                        while (!pending.empty()) {
                            const auto at = pending.back();
                            pending.pop_back();
                            if (known->second[at]) continue;
                            known->second[at] = true;
                            pending.insert(pending.end(), successors[at].begin(), successors[at].end());
                        }
                    }
                    for (const auto after : current->second) {
                        ++checked;
                        if (!known->second[after])
                            violations.push_back({
                                { "before_node", before },
                                {  "after_node",  after }
                            });
                    }
                }
            }
            else if (previous != intervals.end()) ++overlapping_groups;
            if (previous == intervals.end() || current->first.second >= previous->first.second) previous = current;
        }
    }
    return {
        {            "checked_order_pairs",checked                                           },
        {                     "violations", std::move(violations) },
        { "unqualified_overlapping_groups",    overlapping_groups },
        {                  "qualification",
         "Actual same-thread nonoverlapping CPU/closed-phase/file service order projected onto matched nodes; grouped remainders excluded. Device/data, cache "
         "and within-parent orders are not "
         "certified."                                            }
    };
}

} // namespace

PreparedEvaluation prepare(const std::filesystem::path & input) {
    const auto request = read(input);
    auto prediction = adapters::read_trajectory(request.at("prediction").get<std::string>(), request.at("target").get<std::string>());
    auto source_manifest = source::inspect_manifest(prediction.source_manifest);
    const auto target_manifest = source::inspect_manifest(request.at("manifest").get<std::string>());
    const auto base = read(source_manifest.configuration_path.value());
    const auto target = read(target_manifest.configuration_path.value());
    if (prediction.deployment != target.at("deployment") || prediction.deployment != base.at("deployment")
        || source_manifest.framework != target_manifest.framework || base.at("workload") != target.at("workload"))
        throw std::runtime_error("Target evaluation requires the predicted deployment and the base's exact encoded workload");
    for (const auto & [name, value] : prediction.cell.at("configuration").items())
        if (target.at("server").at("arguments").at(name) != value) throw std::runtime_error("Target profile does not use predicted setting: " + name);
    return { std::move(prediction), std::move(source_manifest), adapters::prepare_source(target_manifest, std::nullopt, adapters::SourceDetail::Regions) };
}

Json report(const PreparedEvaluation & input) {
    const auto & trajectory = input.prediction;
    const auto & cell = trajectory.cell;
    const auto & serialized = cell.at("execution_graph");
    const auto & nodes = serialized.at("nodes");
    const auto & observed = input.target.context.observations();
    std::map<std::string, std::size_t> requests;
    for (std::size_t id = 0; id < observed.requests.size(); ++id) requests.emplace(observed.requests[id].identity, id);
    if (cell.at("requests").size() != requests.size()) throw std::runtime_error("Predicted and target request boundaries differ");
    for (std::size_t id = 0; id < observed.requests.size(); ++id)
        if (cell.at("requests").at(id).at("request") != observed.requests[id].identity
            || cell.at("requests").at(id).at("formal") != observed.requests[id].formal)
            throw std::runtime_error("Predicted and target request identities/formal boundary differ");

    std::map<Address, std::vector<Measurement>> measured;
    for (const auto & cost : observed.cost_observations)
        measured[{ cost.request ? std::optional(requests.at(observed.names[*cost.request])) : std::nullopt,
                   cost.iteration,
                   cost.participant,
                   std::string(cost.port),
                   cost.work,
                   cost.data }]
            .push_back({ cost.duration_ns, &cost });
    const auto & regions = input.target.context.regions();
    for (std::size_t id = 0; id < observed.requests.size(); ++id) {
        const auto & window = observed.requests[id];
        if (window.received) {
            const auto ns = observed.points[*window.received].time_ns - window.begin_ns;
            if (ns >= 0) measured[{ id, 0, std::nullopt, "ingress", std::nullopt }].push_back({ ns, nullptr });
        }
        if (window.sent) {
            const auto ns = window.end_ns - observed.points[*window.sent].time_ns;
            if (ns >= 0) measured[{ id, 0, std::nullopt, "egress", std::nullopt }].push_back({ ns, nullptr });
        }
        const auto & ports = regions.request(id);
        for (const auto & [index, name] : { std::pair(ports.before_execution, "before_execution"), std::pair(ports.after_execution, "after_execution") }) {
            if (!index) continue;
            const auto & region = regions.residual(*index).observation;
            measured[{ id, 0, 0, name, std::nullopt }].push_back({ region.end_ns - region.begin_ns, nullptr });
        }
    }

    adapters::PreparedReplay replay{ trajectory, {}, {}, std::nullopt };
    const auto original = commands::trajectory_replay_report(replay);
    if (original.at("operation_timing_differences") != 0) throw std::runtime_error("Recorded path does not reproduce its original operation times");
    std::vector<std::optional<Address>> addresses;
    std::map<Address, std::size_t> counts;
    for (const auto & node : nodes) {
        addresses.push_back(node.at("kind") == "boundary" ? std::nullopt : address(node));
        if (addresses.back()) ++counts[*addresses.back()];
    }
    auto missing = Json::array(), matches = Json::array();
    std::set<Address> consumed;
    std::vector<std::pair<const source::ObservedCost *, std::size_t>> ordered;
    std::size_t replaceable = 0;
    for (std::size_t id = 0; id < nodes.size(); ++id) {
        const auto & node = nodes[id];
        if (node.at("kind") == "boundary" || node.at("origin").at("type") == "relative_deadline") continue;
        ++replaceable;
        const auto & key = addresses[id];
        const auto candidates = key ? measured.find(*key) : measured.end();
        std::string issue;
        if (!key) issue = "semantic_address_unavailable";
        else if (key->work && !key->participant) issue = "participant_unobserved";
        else if (counts.at(*key) != 1) issue = "ambiguous_predicted_address";
        else if (candidates == measured.end()) issue = "target_work_or_scope_unobserved";
        else if (candidates->second.size() != 1) issue = "ambiguous_target_address";
        if (!issue.empty()) {
            missing.push_back({
                {   "node",                id },
                {  "issue",             issue },
                { "origin", node.at("origin") },
                {   "work",   node.at("work") }
            });
            if (node.contains("position")) missing.back()["position"] = node.at("position");
            continue;
        }
        const auto & cost = candidates->second.front();
        replay.overrides.emplace(id, cost.duration_ns);
        consumed.insert(*key);
        matches.push_back({
            {                  "node",                     id },
            { "predicted_duration_ns", node.at("duration_ns") },
            {    "target_duration_ns",       cost.duration_ns }
        });
        if (cost.observation) {
            const auto & record = cost.observation->region.record;
            matches.back()["target_record"] = {
                {     "file", input.target.manifest.files[record.file].path.string() },
                { "position",                                        record.position }
            };
            ordered.emplace_back(cost.observation, id);
        }
    }
    auto unused = Json::array();
    for (const auto & [key, values] : measured)
        if (!consumed.contains(key)) {
            unused.push_back({
                {      "request",          key.request ? Json(*key.request) : Json(nullptr) },
                {    "iteration",                                             key.iteration },
                {  "participant",  key.participant ? Json(*key.participant) : Json(nullptr) },
                {         "port",                                                  key.port },
                {         "work", key.work ? commands::work_json(*key.work) : Json(nullptr) },
                { "observations",                                             values.size() }
            });
            if (key.data) unused.back()["data"] = *key.data;
        }
    const auto replacement = commands::trajectory_replay_report(replay);
    std::optional<graph::Time> begin, end;
    graph::Time request_sum_ns = 0;
    for (const auto & request : observed.requests)
        if (request.formal) {
            if (!begin) begin = request.begin_ns;
            end = request.end_ns;
            request_sum_ns += request.end_ns - request.begin_ns;
        }
    const auto target_sec = begin && end ? Json((*end - *begin) / 1e9) : Json(nullptr);
    const auto order = order_report(serialized, ordered);
    return {
        {                                      "status",                             "completed"                                                        },
        {                                  "prediction",                                                                  trajectory.prediction.string() },
        {                                      "target",                                                                                 cell.at("name") },
        {                             "target_manifest",                                                             input.target.manifest.path.string() },
        {                    "target_profiled_http_sec",                                                                                      target_sec },
        {             "target_profiled_request_sum_sec",                                                                            request_sum_ns / 1e9 },
        { "target_profiled_client_between_requests_sec",                     begin && end ? Json((*end - *begin - request_sum_ns) / 1e9) : Json(nullptr) },
        {                 "original_predicted_http_sec",                                                                      cell.at("formal_http_sec") },
        {                    "original_replay_http_sec",                                                                original.at("formal_replay_sec") },
        {                      "oracle_replay_http_sec",                  replay.overrides.empty() ? Json(nullptr) : replacement.at("formal_replay_sec") },
        {                              "fully_replaced",                                      replaceable != 0 && replay.overrides.size() == replaceable },
        {                               "cost_coverage",
         { { "observed_response_nodes", observed.cost_observations.size() },
         { "replaceable_nodes", replaceable },
         { "replaced_nodes", replay.overrides.size() },
         { "retained_nodes", missing.size() },
         { "matches", std::move(matches) },
         { "unmatched_predicted", std::move(missing) },
         { "unmatched_observed", std::move(unused) } }                                                                                                  },
        {                              "observed_order",                                                                                           order },
        {                               "qualification",
         "Independent evaluator only. Exact request/iteration/participant, work and scope matching; unresolved costs retain the original estimate. Replay "
         "fixes the executed path and does not regenerate branches. Target is profiled HTTP, not normal E2E. Semantic/cpu order coverage is not a complete "
         "cache or device-causality certificate. The client bookkeeping gap remains in target HTTP but is not represented by immediate serial arrival." }
    };
}

} // namespace markov::evaluation
