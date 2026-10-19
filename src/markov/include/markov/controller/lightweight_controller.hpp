#pragma once

#include "markov/contracts/control.hpp"

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
    ControllerSummary finish() const override;

private:
    enum class RequestPhase { NotReceived, Waiting, Active, Finished };

    struct Progress {
        RequestPhase phase = RequestPhase::NotReceived;
        std::uint32_t published_tokens = 0;
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
    std::deque<std::uint32_t> free_request_slots_;
    std::deque<std::uint32_t> free_pages_;
    std::deque<RequestId> incoming_;
    std::deque<RequestId> waiting_;

    struct PendingBatch {
        BatchInput input;
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
