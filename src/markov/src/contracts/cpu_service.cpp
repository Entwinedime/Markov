#include "markov/contracts/cpu_service.hpp"

#include "markov/contracts/control.hpp"
#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace markov {
namespace {

constexpr std::array<std::string_view, 8> kinds{ "scope", "function", "enqueue", "worker", "runtime", "wait", "transfer", "internal_wait" };

} // namespace

std::string_view CpuBoundary::role_name(CpuThreadRole role) { return role == CpuThreadRole::Control ? "control" : "worker"; }
std::string_view CpuBoundary::kind_name(CpuBoundaryKind kind) { return kinds[std::to_underlying(kind)]; }

CpuBoundary CpuBoundary::read(const nlohmann::json & value) {
    input::fields(value, { "role", "kind", "edge", "name", "ordinal", "count" });
    const auto role = input::string_value(value.at("role"));
    const auto edge = input::string_value(value.at("edge"));
    if ((role != "control" && role != "worker") || (edge != "begin" && edge != "end")) throw std::invalid_argument("invalid CPU boundary role or edge");

    const auto kind = input::string_value(value.at("kind"));
    const auto found = std::ranges::find(kinds, kind);
    if (found == kinds.end()) throw std::invalid_argument("unknown CPU boundary kind: " + kind);

    const auto count = input::integer(value.at("count"), 1, std::numeric_limits<std::int64_t>::max());
    return { role == "control" ? CpuThreadRole::Control : CpuThreadRole::Worker,
             static_cast<CpuBoundaryKind>(found - kinds.begin()),
             edge == "begin" ? CpuBoundaryEdge::Begin : CpuBoundaryEdge::End,
             input::string_value(value.at("name")),
             static_cast<std::size_t>(input::integer(value.at("ordinal"), 0, count - 1)),
             static_cast<std::size_t>(count) };
}

nlohmann::json CpuBoundary::describe() const {
    return {
        {    "role",                                  role_name(role) },
        {    "kind",                                  kind_name(kind) },
        {    "edge", edge == CpuBoundaryEdge::Begin ? "begin" : "end" },
        {    "name",                                             name },
        { "ordinal",                                          ordinal },
        {   "count",                                            count }
    };
}

CpuServiceDescriptor CpuServiceDescriptor::read(const nlohmann::json & value) {
    input::fields(value, { "begin", "end" });
    auto result = CpuServiceDescriptor{ CpuBoundary::read(value.at("begin")), CpuBoundary::read(value.at("end")) };
    if (result.begin.role != result.end.role) throw std::invalid_argument("CPU service boundaries must belong to the same thread role");
    return result;
}

nlohmann::json CpuServiceDescriptor::describe() const {
    return {
        { "begin", begin.describe() },
        {   "end",   end.describe() }
    };
}

CpuScheduleWork CpuScheduleWork::read(const nlohmann::json & value) {
    input::fields(value, { "transition", "waiting_requests", "filters", "merge", "incoming_requests" });
    const auto & transition = value.at("transition");
    input::fields(transition, { "running_requests", "pending_prefill_requests", "admission_blocked" });
    const auto count = [](const nlohmann::json & number) {
        return static_cast<std::size_t>(input::integer(number, 0, std::numeric_limits<std::int64_t>::max()));
    };
    CpuScheduleWork result{
        { count(transition.at("running_requests")), std::nullopt, transition.at("admission_blocked").get<bool>() },
        count(value.at("waiting_requests")),
        {},
        std::nullopt,
        {}
    };
    if (!transition.at("pending_prefill_requests").is_null()) result.transition.pending_prefill_requests = count(transition.at("pending_prefill_requests"));
    for (const auto & filter : value.at("filters")) {
        if (filter.size() != 2) throw std::invalid_argument("CPU filter work requires input and retained counts");
        const auto input_count = count(filter.at(0));
        result.filters.emplace_back(input_count, input::integer(filter.at(1), 0, input_count));
    }
    const auto & merge = value.at("merge");
    if (!merge.is_null()) {
        input::fields(merge, { "left_count", "right_count" });
        result.merge = BatchMerge{ count(merge.at("left_count")), count(merge.at("right_count")) };
    }
    for (const auto & request : value.at("incoming_requests")) {
        if (request.size() != 2) throw std::invalid_argument("CPU incoming work requires prompt and output token counts");
        result.incoming_requests.emplace_back(input::integer(request.at(0), 0, std::numeric_limits<std::uint32_t>::max()),
                                              input::integer(request.at(1), 0, std::numeric_limits<std::uint32_t>::max()));
    }
    return result;
}

nlohmann::json CpuScheduleWork::describe() const {
    return {
        {        "transition",                               transition.describe() },
        {  "waiting_requests",                                    waiting_requests },
        {           "filters",                                             filters },
        {             "merge", merge ? merge->describe() : nlohmann::json(nullptr) },
        { "incoming_requests",                                   incoming_requests }
    };
}

CpuPublicationWork CpuPublicationWork::read(const nlohmann::json & value) {
    input::fields(value, { "requests" });
    CpuPublicationWork result;
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    for (const auto & request : value.at("requests")) {
        input::fields(request, { "output_tokens", "decode_context_tokens", "history_tokens" });
        result.requests.push_back({ static_cast<std::uint64_t>(input::integer(request.at("output_tokens"), 0, maximum)),
                                    static_cast<std::uint64_t>(input::integer(request.at("decode_context_tokens"), 0, maximum)),
                                    static_cast<std::uint64_t>(input::integer(request.at("history_tokens"), 0, maximum)) });
    }
    return result;
}

nlohmann::json CpuPublicationWork::describe() const {
    auto members = nlohmann::json::array();
    for (const auto & request : requests)
        members.push_back({
            {         "output_tokens",         request.output_tokens },
            { "decode_context_tokens", request.decode_context_tokens },
            {        "history_tokens",        request.history_tokens }
        });
    return {
        { "requests", std::move(members) }
    };
}

CpuServiceWork CpuServiceWork::from_input(CpuServiceDescriptor service, StepKind step, std::uint32_t rank, const BatchInput & input,
                                          std::optional<CpuPublicationWork> publication) {
    CpuServiceWork result{ std::move(service), step, input.phase, rank, input.requests.size(), 0, 0, 0 };
    result.publication = std::move(publication);
    for (const auto & request : input.requests) {
        result.input_tokens += request.input_tokens;
        result.context_tokens += request.context_tokens;
        if (step == StepKind::Publish) result.finished_requests += request.finishes;
    }
    if (step == StepKind::Schedule) {
        result.scheduling = CpuScheduleWork{ input.transition, input.admission.waiting_requests, {}, input.merge, {} };
        for (const auto & filter : input.filters) result.scheduling->filters.emplace_back(filter.input_count, filter.retained_indices.size());
        for (const auto & request : input.incoming_requests) result.scheduling->incoming_requests.emplace_back(request.prompt_tokens, request.output_tokens);
    }
    return result;
}

CpuServiceWork CpuServiceWork::read(const nlohmann::json & value) {
    input::fields(
        value,
        { "service", "step", "phase", "rank", "requests", "input_tokens", "context_tokens", "finished_requests", "scheduling", "collective", "publication" });
    const ServingContract contract;
    const auto phase = input::string_value(value.at("phase"));
    if (phase != "prefill" && phase != "decode" && phase != "idle") throw std::invalid_argument("invalid CPU work phase");

    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto requests = input::integer(value.at("requests"), 0, maximum);
    CpuServiceWork result{ CpuServiceDescriptor::read(value.at("service")),
                           contract.block(input::string_value(value.at("step"))).kind,
                           phase == "prefill"  ? BatchPhase::Prefill
                           : phase == "decode" ? BatchPhase::Decode
                                               : BatchPhase::Idle,
                           static_cast<std::uint32_t>(input::integer(value.at("rank"), 0, std::numeric_limits<std::uint32_t>::max())),
                           static_cast<std::uint64_t>(requests),
                           static_cast<std::uint64_t>(input::integer(value.at("input_tokens"), 0, maximum)),
                           static_cast<std::uint64_t>(input::integer(value.at("context_tokens"), 0, maximum)),
                           static_cast<std::uint64_t>(input::integer(value.at("finished_requests"), 0, requests)) };
    if (!value.at("scheduling").is_null()) result.scheduling = CpuScheduleWork::read(value.at("scheduling"));
    if (!value.at("collective").is_null()) result.collective = CollectiveWork::read(value.at("collective"));
    if (!value.at("publication").is_null()) result.publication = CpuPublicationWork::read(value.at("publication"));
    if (result.scheduling.has_value() != (result.step == StepKind::Schedule))
        throw std::invalid_argument("CPU scheduling work must be supplied exactly for Schedule");
    if (result.publication && (result.step != StepKind::Publish || result.publication->requests.size() != result.requests))
        throw std::invalid_argument("CPU publication work must describe the ordered Publish members");
    return result;
}

nlohmann::json CpuServiceWork::describe() const {
    const ServingContract contract;
    return {
        {           "service",                                              service.describe() },
        {              "step",                                       contract.block(step).name },
        {             "phase",                                      contract.phase_name(phase) },
        {              "rank",                                                            rank },
        {          "requests",                                                        requests },
        {      "input_tokens",                                                    input_tokens },
        {    "context_tokens",                                                  context_tokens },
        { "finished_requests",                                               finished_requests },
        {        "scheduling",   scheduling ? scheduling->describe() : nlohmann::json(nullptr) },
        {        "collective",   collective ? collective->describe() : nlohmann::json(nullptr) },
        {       "publication", publication ? publication->describe() : nlohmann::json(nullptr) }
    };
}

} // namespace markov
