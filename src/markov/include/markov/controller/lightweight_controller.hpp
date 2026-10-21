#pragma once

#include "markov/contracts/control.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace markov {

// FCFS, separate unchunked prefill/decode, one batch in flight. The producer
// qualifies the completion-horizon policy and supplies its effective parameters.
class LightweightController final : public Controller {
public:
    LightweightController(std::uint32_t page_size_tokens, AdmissionPolicy policy);

    void initialize(std::span<const Request> requests, ServingLimits limits) override;
    std::vector<ControlAction> handle(std::span<const ControlEvent> events) override;
    std::vector<PortRef> retained_ports() const override;
    ControllerSummary finish() const override;

private:
    struct Progress {
        std::uint32_t generated_tokens = 0;
        std::optional<std::uint32_t> pool_slot;
        std::vector<std::uint32_t> kv_pages;
    };

    BatchInput select_batch();
    BatchFilter filter_members(std::vector<RequestId> & members);
    void admit_prefill(BatchInput & batch);
    std::vector<ControlAction> invoke(BatchInput input);

    std::span<const Request> requests_; // Owned by the workflow for the full run.
    ServingLimits limits_{};
    std::uint32_t page_size_tokens_;
    AdmissionPolicy admission_policy_;
    double admission_ratio_;

    std::vector<Progress> progress_;
    std::size_t completed_requests_ = 0;
    std::deque<std::uint32_t> free_request_slots_;
    std::deque<std::uint32_t> free_pages_;
    std::deque<RequestId> incoming_;
    std::deque<RequestId> waiting_;

    struct PendingBatch {
        std::vector<RequestId> members; // Empty for an Idle scheduling decision.
        PortRef completion;
    };
    std::optional<PendingBatch> reserved_batch_;
    std::optional<PortRef> previous_completion_;
    WorkId next_work_id_ = 1;
    std::vector<RequestId> running_members_;
    std::optional<std::vector<RequestId>> pending_prefill_;
    bool admission_blocked_ = false;
};

} // namespace markov
