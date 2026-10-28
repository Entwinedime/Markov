#include "markov/contracts/serving.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace markov {

std::vector<Request> Request::ordered(std::vector<Request> requests) {
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
    case ServingStep::Stage:
        return "stage";
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
    }
    throw std::logic_error("unknown serving step");
}

} // namespace markov
