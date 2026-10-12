#include "markov/controller/lightweight_controller.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

namespace markov {

LightweightController::LightweightController(std::uint32_t page_size_tokens, AdmissionPolicy policy)
    : page_size_tokens_(page_size_tokens),
      admission_policy_(policy),
      admission_ratio_(policy.initial_ratio) {}

void LightweightController::initialize(std::span<const Request> requests, ServingLimits limits) {
    requests_ = requests;
    limits_ = limits;

    progress_.assign(requests.size(), Progress{});
    free_request_slots_.clear();
    for (std::uint32_t slot = 1; slot <= limits_.request_capacity; ++slot) free_request_slots_.push_back(slot);

    free_pages_.clear();
    const auto pages = limits_.kv_capacity_tokens / page_size_tokens_;
    for (std::uint32_t page = 1; page <= pages; ++page) free_pages_.push_back(page);

    arrival_order_.clear();
    reserved_batch_.reset();
    previous_completion_.reset();

    next_work_id_ = 1;
    awaited_completion_.reset();
    running_members_.clear();
    pending_prefill_.reset();
    admission_blocked_ = false;
    admission_ratio_ = admission_policy_.initial_ratio;
}

std::vector<ControlAction> LightweightController::handle(std::span<const ControlEvent> events, const RuntimeView &) {
    std::vector<ControlAction> actions;
    for (const auto & event : events) {
        if (const auto * arrival = std::get_if<RequestArrived>(&event.payload)) {
            progress_[arrival->request_id].phase = RequestPhase::Waiting;
            arrival_order_.push_back(arrival->request_id);
            continue;
        }

        const auto * completion = std::get_if<WorkPortCompleted>(&event.payload);
        if (!completion || !awaited_completion_ || completion->port.work_id != awaited_completion_->work_id
            || completion->port.port != awaited_completion_->port) {
            continue;
        }

        // Only the publication port changes token visibility or frees logical KV.
        for (const auto & slice : reserved_batch_->requests) {
            auto & progress = progress_[slice.request_id];
            ++progress.published_tokens;
            if (progress.published_tokens == requests_[slice.request_id].output_tokens) {
                progress.phase = RequestPhase::Finished;
                free_request_slots_.push_back(*progress.pool_slot);
                progress.pool_slot.reset();
                // Pages are logical identities. Reuse order does not reproduce
                // a backend free-list tensor; its operations remain in templates.
                free_pages_.insert(free_pages_.end(), progress.kv_pages.begin(), progress.kv_pages.end());
                progress.kv_pages.clear();
                actions.emplace_back(CompleteRequest{ slice.request_id, completion->port });
            }
            else { progress.phase = RequestPhase::Active; }
        }

        previous_completion_ = completion->port;

        if (reserved_batch_->phase == BatchPhase::Idle && std::ranges::none_of(progress_, [](const auto & request) {
                return request.phase == RequestPhase::Waiting || request.phase == RequestPhase::Active;
            }))
            admission_ratio_ = admission_policy_.initial_ratio;

        reserved_batch_.reset();
    }

    if (reserved_batch_) { return actions; }

    if (finish().completed_requests == requests_.size()) return actions;

    auto batch = select_batch();
    if (batch.requests.empty() && batch.filters.empty()) { return actions; }

    // Admission is an estimate. Check actual page demand before mutating either
    // request slots or page ownership; retraction is outside this policy.
    std::uint64_t needed_pages = 0;
    for (const auto & request : batch.requests) {
        const auto tokens = static_cast<std::uint64_t>(request.context_tokens) + request.input_tokens;
        const auto pages = (tokens + page_size_tokens_ - 1) / page_size_tokens_;
        needed_pages += pages - progress_[request.request_id].kv_pages.size();
    }
    if (needed_pages > free_pages_.size()) {
        actions.emplace_back(UnsupportedControl{ "kv_retraction_not_implemented" });
        return actions;
    }
    if (batch.phase == BatchPhase::Decode) admission_ratio_ = std::max(admission_ratio_ - admission_policy_.decay_per_decode, admission_policy_.minimum_ratio);

    auto invocations = invoke(std::move(batch));
    for (auto & invocation : invocations) { actions.push_back(std::move(invocation)); }

    return actions;
}

BatchInput LightweightController::select_batch() {
    const BatchTransitionState transition{ running_members_.size(),
                                           pending_prefill_ ? std::optional(pending_prefill_->size()) : std::nullopt,
                                           admission_blocked_ };
    BatchInput batch{ BatchPhase::Prefill, {}, free_pages_.size(), transition, {}, {}, std::nullopt, {} };
    if (pending_prefill_) {
        batch.filters.push_back(filter_members(*pending_prefill_));
        if (pending_prefill_->size() < batch.filters.back().input_count) admission_blocked_ = false;
        // Adopting a fresh prefill batch replaces the previous batch's latch.
        // Merging into a nonempty running batch preserves it.
        if (running_members_.empty() && !pending_prefill_->empty()) admission_blocked_ = false;
        if (!running_members_.empty() && !pending_prefill_->empty()) batch.merge = BatchMerge{ running_members_.size(), pending_prefill_->size() };
        running_members_.insert(running_members_.end(), pending_prefill_->begin(), pending_prefill_->end());
        pending_prefill_.reset();
    }

    batch.admission = { static_cast<std::size_t>(std::ranges::count_if(progress_, [](const auto & request) { return request.phase == RequestPhase::Waiting; })),
                        running_members_.size(),
                        admission_blocked_ };
    admit_prefill(batch);
    if (!batch.requests.empty()) { return batch; }

    // Native admission precedes filtering the running batch. A full finished
    // batch therefore produces a real empty decision before another admission.
    if (!running_members_.empty()) {
        batch.filters.push_back(filter_members(running_members_));
        // Publication releases logical resources, but reopening admission occurs
        // only when this later Schedule observes the removed batch members.
        if (running_members_.size() < batch.filters.back().input_count) admission_blocked_ = false;
    }
    batch.phase = running_members_.empty() ? BatchPhase::Idle : BatchPhase::Decode;
    for (const auto request_id : running_members_) {
        const auto prompt_tokens = std::get<std::vector<std::uint32_t>>(requests_[request_id].input).size();
        const auto context_tokens = prompt_tokens + progress_[request_id].published_tokens - 1;
        batch.requests.push_back({ request_id,
                                   1,
                                   static_cast<std::uint32_t>(context_tokens),
                                   progress_[request_id].published_tokens,
                                   progress_[request_id].published_tokens + 1 == requests_[request_id].output_tokens,
                                   0,
                                   {} });
    }

    return batch;
}

BatchFilter LightweightController::filter_members(std::vector<RequestId> & members) {
    BatchFilter filter{ members.size(), {} };
    std::vector<RequestId> retained;
    for (std::size_t slot = 0; slot < members.size(); ++slot) {
        if (progress_[members[slot]].phase == RequestPhase::Finished) continue;
        filter.retained_indices.push_back(slot);
        retained.push_back(members[slot]);
    }
    members = std::move(retained);
    return filter;
}

void LightweightController::admit_prefill(BatchInput & batch) {
    auto & work = batch.admission_work;
    if (admission_blocked_) {
        work.stop = AdmissionStop::Closed;
        return;
    }
    if (batch.admission.waiting_requests == 0) {
        work.stop = AdmissionStop::NoWaitingRequests;
        return;
    }
    if (running_members_.size() == limits_.request_capacity) {
        admission_blocked_ = true;
        work.stop = AdmissionStop::RequestCapacity;
        return;
    }

    struct Horizon {
        std::uint64_t remaining;
        std::uint64_t occupied;
    };
    const auto horizon = [this](RequestId id) {
        return Horizon{ requests_[id].output_tokens - progress_[id].published_tokens,
                        std::get<std::vector<std::uint32_t>>(requests_[id].input).size() + progress_[id].published_tokens };
    };

    const auto available = static_cast<std::int64_t>(free_pages_.size() * page_size_tokens_);
    std::int64_t current_offset = 0;
    double total_offset = 0;
    for (const auto id : running_members_)
        total_offset += std::min<std::uint64_t>(horizon(id).remaining, admission_policy_.output_estimate_limit_tokens) * admission_ratio_;

    std::vector<Horizon> horizons;
    std::uint64_t prefill_tokens = 0;
    for (const auto id : arrival_order_) {
        if (progress_[id].phase != RequestPhase::Waiting) continue;
        ++work.candidate_checks;
        if (running_members_.size() + batch.requests.size() == limits_.request_capacity) {
            admission_blocked_ = true;
            work.stop = AdmissionStop::RequestCapacity;
            break;
        }

        work.request_horizon_checks.push_back(0);
        const auto & request = requests_[id];
        const auto tokens = std::get<std::vector<std::uint32_t>>(request.input).size();
        const auto paged = static_cast<std::int64_t>((tokens + page_size_tokens_ - 1) / page_size_tokens_ * page_size_tokens_);
        if (paged > available - current_offset || paged > available - total_offset) {
            admission_blocked_ = true;
            work.stop = AdmissionStop::MemoryBudget;
            break;
        }

        const auto candidate = horizon(id);
        if (horizons.empty()) {
            horizons.push_back(candidate);
            for (const auto running : running_members_) {
                const auto value = horizon(running);
                if (value.remaining > 0) horizons.push_back(value);
            }
            std::stable_sort(horizons.begin(), horizons.end(), [](const auto & left, const auto & right) { return left.remaining < right.remaining; });
        }
        else {
            auto position = std::find_if(horizons.begin(), horizons.end(), [&candidate](const auto & item) { return candidate.remaining <= item.remaining; });
            // This qualified policy inserts before the terminal item when no
            // earlier position matches. Re-sorting would change its decisions.
            if (position == horizons.end()) --position;
            horizons.insert(position, candidate);
        }

        std::int64_t freed = 0;
        for (std::size_t index = 0; index < horizons.size(); ++index) {
            ++work.request_horizon_checks.back();
            const auto remaining = static_cast<std::int64_t>(horizons.size() - index);
            const auto & item = horizons[index];
            const auto minimum_free = available - current_offset - paged + freed - static_cast<std::int64_t>(item.remaining) * remaining;
            if (minimum_free <= static_cast<std::int64_t>(admission_policy_.reserve_tokens_per_request) * remaining) {
                admission_blocked_ = true;
                work.stop = AdmissionStop::MemoryBudget;
                break;
            }
            freed += static_cast<std::int64_t>(item.occupied);
        }
        if (admission_blocked_) break;

        batch.requests.push_back({ id, static_cast<std::uint32_t>(tokens), 0, 0, request.output_tokens == 1, 0, {} });
        current_offset += paged + page_size_tokens_;
        total_offset += paged + std::min(request.output_tokens, admission_policy_.output_estimate_limit_tokens) + page_size_tokens_;
        prefill_tokens += paged;

        // Memory exhaustion closes future admission; reaching the input budget
        // merely ends this prefill batch after committing the current request.
        if (available - current_offset <= 0 || available - total_offset <= 0) {
            admission_blocked_ = true;
            work.stop = AdmissionStop::MemoryBudget;
            break;
        }
        if (prefill_tokens >= limits_.prefill_capacity_tokens) {
            work.stop = AdmissionStop::InputBudget;
            break;
        }
    }
}

std::vector<ControlAction> LightweightController::invoke(BatchInput input) {
    // Reserve a logical FIFO slot once. Non-overlap admission guarantees
    // capacity; all four actions carry the same immutable slot snapshot.
    for (auto & slice : input.requests) {
        auto & progress = progress_[slice.request_id];
        if (input.phase == BatchPhase::Prefill) {
            progress.pool_slot = free_request_slots_.front();
            free_request_slots_.pop_front();
        }
        slice.pool_slot = *progress.pool_slot;

        // Reserve only newly needed pages, not the full admission token budget.
        const auto sequence_tokens = static_cast<std::uint64_t>(slice.context_tokens) + slice.input_tokens;
        const auto page_count = (sequence_tokens + page_size_tokens_ - 1) / page_size_tokens_;
        while (progress.kv_pages.size() < page_count) {
            progress.kv_pages.push_back(free_pages_.front());
            free_pages_.pop_front();
        }
        slice.kv_pages = progress.kv_pages;
    }

    const auto schedule_id = next_work_id_++;
    std::vector<PortRef> schedule_dependencies;
    if (previous_completion_) schedule_dependencies.push_back(*previous_completion_);
    reserved_batch_ = input;

    if (input.phase == BatchPhase::Idle) {
        awaited_completion_ = PortRef{ schedule_id, PortKind::Selected };
        return {
            InvokeStep{ schedule_id, StepKind::Schedule, input, std::move(schedule_dependencies), {} }
        };
    }

    const auto prepare_id = next_work_id_++;
    const auto execute_id = next_work_id_++;
    const auto publish_id = next_work_id_++;
    const auto execute_step = input.phase == BatchPhase::Prefill ? StepKind::Prefill : StepKind::Decode;
    awaited_completion_ = PortRef{ publish_id, PortKind::Published };
    if (input.phase == BatchPhase::Prefill) {
        pending_prefill_.emplace();
        for (const auto & request : input.requests) pending_prefill_->push_back(request.request_id);
    }

    return {
        InvokeStep{ schedule_id,StepKind::Schedule,input,        std::move(schedule_dependencies),{}                                                                                                    },
        InvokeStep{  prepare_id,  StepKind::Prepare, input, { { schedule_id, PortKind::Selected } },                                  {} },
        InvokeStep{  execute_id,       execute_step, input,  { { prepare_id, PortKind::Prepared } },                                  {} },
        InvokeStep{  publish_id,
                   StepKind::Publish,
                   input, { { execute_id, PortKind::Submitted } },
                   { { { execute_id, PortKind::DeviceDone }, PortKind::Published } }                                                    }
    };
}

ControllerSummary LightweightController::finish() const {
    std::size_t completed_requests = 0;
    for (const auto & progress : progress_) {
        if (progress.phase == RequestPhase::Finished) { ++completed_requests; }
    }

    return { completed_requests };
}

} // namespace markov
