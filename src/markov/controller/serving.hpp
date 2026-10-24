#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "markov/contracts/serving.hpp"

namespace markov {

// Planning is read-only; start and finish commit allocation and completion effects.
class ServingController {
public:
    ServingController(ServingSettings settings, std::vector<Request> requests);

    std::optional<Batch> plan(std::int64_t now_ns, AdmissionConstraints admission) const;

    // Submit the current plan only after its target graph has been assembled successfully.
    void start(Batch batch);
    BatchEffects effects(const Batch & batch) const;
    void finish(const BatchEffects & effects, std::span<const std::size_t> returned_pages, std::int64_t completed_ns);

    std::optional<std::int64_t> next_arrival_ns(std::int64_t now_ns) const;
    bool done() const;
    std::size_t free_pages() const;
    std::size_t request_count() const;
    const Request & request(std::size_t identity) const;

private:
    struct RequestState {
        Request input;
        std::int64_t peak_pages;

        std::optional<std::size_t> slot;
        std::int64_t resident_tokens = 0;
        std::int64_t generated_tokens = 0;
        std::vector<std::size_t> pages;
    };

    std::int64_t pages_for(std::int64_t tokens) const;

    ServingSettings settings_;
    std::vector<RequestState> requests_;

    std::vector<std::size_t> active_;
    std::deque<std::size_t> free_slots_;
    std::deque<std::size_t> free_pages_;

    std::size_t waiting_ = 0;
    std::int64_t last_event_ns_ = 0;
    std::optional<Batch> in_flight_;
};

} // namespace markov
