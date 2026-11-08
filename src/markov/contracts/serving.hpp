#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/contracts/expression.hpp"

namespace markov {

struct Request {
    std::string id;
    std::int64_t arrival_ns;
    std::int64_t prompt_tokens;
    std::int64_t output_tokens;
    std::optional<std::string> prefix_id{};

    static std::vector<Request> read(const nlohmann::json & source);
};

struct ServingSettings {
    ServingSettings(std::int64_t request_capacity, std::int64_t token_capacity, std::int64_t page_tokens, std::int64_t prefill_tokens);

    std::int64_t max_running_requests;
    std::int64_t max_total_tokens;
    std::int64_t page_size;
    std::int64_t max_prefill_tokens;
};

// Group-local sequences remain unique when independent controllers share one executor.
struct TaskIdentity {
    std::string group;
    std::size_t sequence;

    auto operator<=>(const TaskIdentity &) const = default;
};

// One native decision names a template action; it does not prescribe a framework-specific step machine.
struct ServingAction {
    TaskIdentity task;
    std::string name;
    nlohmann::json condition;
    Parameters parameters;
    nlohmann::json work;

    // emits_output separates a publication from the later native request-state update.
    nlohmann::json outputs;
};

} // namespace markov
