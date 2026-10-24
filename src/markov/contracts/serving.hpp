#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace markov {

enum class Phase { Prefill, Decode };

struct Request {
    std::string id;
    std::int64_t arrival_ns;
    std::int64_t prompt_tokens;
    std::int64_t output_tokens;
};

struct ServingSettings {
    ServingSettings(std::int64_t request_capacity, std::int64_t token_capacity, std::int64_t page_tokens, std::int64_t prefill_tokens);

    std::int64_t max_running_requests;
    std::int64_t max_total_tokens;
    std::int64_t page_size;
    std::int64_t max_prefill_tokens;
};

// Framework facts constrain admission; the controller retains request and page ownership.
struct AdmissionConstraints {
    std::size_t occupied_members;
    std::int64_t prefill_token_granularity;
};

// Each member carries only the page changes needed for this batch.
struct BatchMember {
    std::size_t request;
    std::size_t slot;
    std::int64_t context_tokens;
    std::int64_t new_tokens;

    std::optional<std::size_t> previous_page;
    std::vector<std::size_t> new_pages;
};

// A batch is a decision snapshot; committing it remains the controller's responsibility.
struct Batch {
    Phase phase;
    std::int64_t scheduled_ns;
    std::int64_t free_pages_before;
    std::int64_t allocated_pages;
    std::vector<BatchMember> members;

    std::int64_t new_tokens() const;
};

struct RequestCompletion {
    std::size_t request;
    std::size_t slot;
    std::int64_t generated_tokens;
    std::int64_t resident_tokens;
    bool finished;
    std::vector<std::size_t> released_pages;
};

// Effects can shape the result graph before execution; the completion event commits them later.
struct BatchEffects {
    std::vector<RequestCompletion> members;
};

// Publication is visible before this batch's result-processing return.
struct BatchProgram {
    std::size_t publication;
    std::vector<std::size_t> completions;
    std::vector<std::size_t> returned_pages;
};

} // namespace markov
