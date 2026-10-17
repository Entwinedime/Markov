#pragma once

#include <nlohmann/json_fwd.hpp>

#include <compare>
#include <cstddef>
#include <optional>
#include <vector>

namespace markov {

// Entry state of one batch transition. An absent pending batch differs from
// an existing empty batch; this describes work without owning request state.
struct BatchTransitionState {
    std::size_t running_requests;
    std::optional<std::size_t> pending_prefill_requests;
    bool admission_blocked;

    auto operator<=>(const BatchTransitionState &) const = default;
    nlohmann::json describe() const;
};

// Admission entry controls both candidate traversal and queue compaction.
// Selected requests alone do not describe this CPU work.
struct AdmissionEntryState {
    std::size_t waiting_requests;
    std::size_t running_requests;
    bool admission_blocked;

    bool operator==(const AdmissionEntryState &) const = default;
    nlohmann::json describe() const;
};

enum class AdmissionStop { QueueExhausted, Closed, NoWaitingRequests, RequestCapacity, MemoryBudget, InputBudget };

// Target work produced by the decision itself, not inferred from elapsed time.
struct AdmissionWork {
    std::size_t candidate_checks = 0;
    // One entry per checked candidate, including zero for an early rejection.
    std::vector<std::size_t> request_horizon_checks;
    AdmissionStop stop = AdmissionStop::QueueExhausted;

    bool operator==(const AdmissionWork &) const = default;
    std::size_t horizon_checks() const;
    bool parameterized_cpu_path(const AdmissionEntryState & entry, std::size_t admitted_requests) const;
    nlohmann::json describe(const AdmissionEntryState & entry, std::size_t admitted_requests) const;
};

// One ordered selection of batch members, shared by control and device work.
struct BatchFilter {
    std::size_t input_count;
    std::vector<std::size_t> retained_indices;

    auto operator<=>(const BatchFilter &) const = default;
    nlohmann::json describe() const;
};

// Native merge appends the retained prefill batch to the running batch.
struct BatchMerge {
    std::size_t left_count;
    std::size_t right_count;

    auto operator<=>(const BatchMerge &) const = default;
    nlohmann::json describe() const;
};

} // namespace markov
