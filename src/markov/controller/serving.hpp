#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "markov/contracts/serving.hpp"
#include "markov/controller/control.hpp"
#include "markov/controller/kv_pool.hpp"

namespace markov {

// Planning is read-only; start and finish commit allocation and completion effects.
class ServingController final : public ServingControl {
public:
    ServingController(ServingSettings settings, std::vector<Request> requests, std::string group, std::size_t stages, std::size_t batch_limit);

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

        std::int64_t generated_tokens = 0;
    };

    struct Flow {
        std::size_t identity;
        std::shared_ptr<const BatchWork> work;
        std::size_t stage;
        std::optional<TaskIdentity> running;
    };

    // Lanes retain request membership across iterations; progress and pages remain in requests_.
    struct Lane {
        std::vector<std::size_t> active;
        std::optional<Flow> flow;
    };

    BatchEffects effects(const Batch & batch) const;
    std::shared_ptr<const BatchWork> plan_batch(std::size_t lane, std::int64_t now_ns, const AdmissionConstraints & admission) const;

    ServingSettings settings_;
    std::string group_;
    std::vector<RequestState> requests_;
    KvPool kv_;

    std::size_t stages_;
    std::size_t batch_limit_;
    std::vector<Lane> lanes_;

    std::size_t waiting_ = 0;
    std::int64_t last_event_ns_ = 0;
    std::size_t next_task_ = 0;
    std::size_t next_flow_ = 0;
};

} // namespace markov
