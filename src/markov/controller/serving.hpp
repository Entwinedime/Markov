#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "markov/contracts/serving.hpp"
#include "markov/controller/control.hpp"

namespace markov {

// Planning is read-only; start and finish commit allocation and completion effects.
class ServingController final : public ServingControl {
public:
    ServingController(ServingSettings settings, std::vector<Request> requests, std::string group);

    std::optional<ServingAction> plan(std::int64_t now_ns, const GroupAdmission & admission) const override;

    // Submit the current plan only after its target graph has been assembled successfully.
    void start(const ServingAction & action) override;
    void finish(const ServingAction & action, std::span<const std::size_t> returned_pages, std::int64_t completed_ns) override;

    std::optional<std::int64_t> next_arrival_ns(std::int64_t now_ns) const override;
    bool done() const override;
    std::size_t free_pages() const override;
    std::size_t request_count() const override;
    const Request & request(std::size_t identity) const override;

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
    BatchEffects effects(const Batch & batch) const;

    ServingSettings settings_;
    std::string group_;
    std::vector<RequestState> requests_;

    std::vector<std::size_t> active_;
    std::deque<std::size_t> free_slots_;
    std::deque<std::size_t> free_pages_;

    std::size_t waiting_ = 0;
    std::int64_t last_event_ns_ = 0;
    std::size_t next_task_ = 0;
    std::optional<TaskIdentity> in_flight_;
};

} // namespace markov
