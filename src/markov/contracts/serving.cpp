#include "markov/contracts/serving.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <stdexcept>

namespace markov {

const char * phase_name(Phase phase) {
    switch (phase) {
    case Phase::Prefill:
        return "prefill";
    case Phase::Decode:
        return "decode";
    case Phase::Mixed:
        return "mixed";
    }
    throw std::logic_error("unknown serving phase");
}

std::vector<Request> Request::read(const nlohmann::json & source) {
    if (!source.is_array() || source.empty()) throw std::invalid_argument("serving needs a nonempty request array");
    constexpr std::array fields{ "id", "arrival_ns", "prompt_tokens", "output_tokens", "prefix_id" };
    std::vector<Request> requests;
    for (const auto & request : source) {
        for (const auto & [name, _] : request.items()) {
            if (std::ranges::find(fields, name) == fields.end()) throw std::invalid_argument("request has unsupported field: " + name);
        }
        for (const auto * field : { "arrival_ns", "prompt_tokens", "output_tokens" }) {
            if (!request.at(field).is_number_integer()) throw std::invalid_argument("serving fields must be integers");
        }
        requests.push_back({ request.at("id").get<std::string>(),
                             request.at("arrival_ns").get<std::int64_t>(),
                             request.at("prompt_tokens").get<std::int64_t>(),
                             request.at("output_tokens").get<std::int64_t>() });
        if (request.contains("prefix_id")) requests.back().prefix_id = request.at("prefix_id").get<std::string>();
    }

    // Stable arrival order gives both simulation and recorded batches the same request identities.
    std::stable_sort(requests.begin(), requests.end(), [](const Request & left, const Request & right) { return left.arrival_ns < right.arrival_ns; });
    std::set<std::string> identities;
    for (const auto & request : requests) {
        if (request.id.empty() || !identities.insert(request.id).second) throw std::invalid_argument("request IDs must be nonempty and unique");
        if (request.arrival_ns < 0 || request.prompt_tokens <= 0 || request.output_tokens <= 0)
            throw std::invalid_argument("request needs nonnegative arrival and positive token lengths: " + request.id);
        if (request.output_tokens - 1 > std::numeric_limits<std::int64_t>::max() - request.prompt_tokens)
            throw std::invalid_argument("request token length overflows: " + request.id);
    }
    return requests;
}

ServingSettings::ServingSettings(std::int64_t requests, std::int64_t tokens, std::int64_t page, std::int64_t prefill)
    : max_running_requests(requests),
      max_total_tokens(tokens),
      page_size(page),
      max_prefill_tokens(prefill) {
    if (requests <= 0 || page <= 0 || tokens < page || prefill <= 0)
        throw std::invalid_argument("serving needs positive request/prefill capacity and at least one KV page");
}

// Prefill can contribute several tokens per member; Decode contributes one.
std::int64_t Batch::new_tokens() const {
    std::int64_t result = 0;
    for (const auto & member : members) result += member.new_tokens;

    return result;
}

std::string_view ServingAction::step_name() const {
    switch (step) {
    case ServingStep::Batch:
        return "batch";
    case ServingStep::Select:
        return "select";
    case ServingStep::Retain:
        return "retain";
    case ServingStep::Stage:
        return "stage";
    case ServingStep::Prepare:
        return "prepare";
    case ServingStep::Compute:
        return "compute";
    case ServingStep::Send:
        return "send";
    case ServingStep::Receive:
        return "receive";
    case ServingStep::Wait:
        return "wait";
    case ServingStep::ReadResult:
        return "read_result";
    case ServingStep::Result:
        return "result";
    case ServingStep::Reserve:
        return "reserve";
    case ServingStep::Model:
        return "model";
    case ServingStep::Transfer:
        return "transfer";
    case ServingStep::Accept:
        return "accept";
    case ServingStep::Release:
        return "release";
    case ServingStep::Indices:
        return "indices";
    }
    throw std::logic_error("unknown serving step");
}

} // namespace markov
