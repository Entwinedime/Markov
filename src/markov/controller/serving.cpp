#include "markov/controller/serving.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace markov {

ServingController::ServingController(ServingSettings settings, std::vector<Request> requests, std::string group, std::size_t stages, std::size_t batch_limit)
    : settings_(settings),
      group_(std::move(group)),
      kv_(requests.size(), settings),
      stages_(stages),
      batch_limit_(batch_limit),
      lanes_(stages) {
    if (stages == 0 || batch_limit == 0) throw std::invalid_argument("serving needs stages and a positive microbatch limit");
    const auto capacity = settings.max_total_tokens / settings.page_size;
    for (auto & request : Request::ordered(std::move(requests))) {

        // Prefill emits the first output; only later outputs append that previous token to KV.
        const auto peak = kv_.pages_for(request.prompt_tokens + (request.output_tokens - 1));
        if (peak > capacity) throw std::invalid_argument("request cannot fit the KV capacity: " + request.id);
        requests_.push_back({ std::move(request), peak, 0 });
    }
}

std::optional<ServingAction> ServingController::plan(std::int64_t now_ns, const GroupAdmission & groups) const {
    if (now_ns < last_event_ns_) throw std::invalid_argument("control time must not move backwards");

    std::vector<bool> busy(stages_ + 1);
    for (const auto & lane : lanes_) {
        if (lane.flow && lane.flow->running) busy[lane.flow->stage] = true;
    }

    // Advance downstream work first; independent stages can still have decisions in flight together.
    std::optional<std::size_t> selected;
    for (std::size_t lane = 0; lane < lanes_.size(); ++lane) {
        const auto & flow = lanes_[lane].flow;
        if (!flow || flow->running || busy[flow->stage]) continue;
        if (!selected || flow->stage > lanes_[*selected].flow->stage) selected = lane;
    }
    if (selected) {
        const auto & flow = *lanes_[*selected].flow;
        const auto stage = flow.stage == stages_ ? std::nullopt : std::optional(flow.stage);
        return ServingAction{
            { group_, next_task_ },
            flow.work, now_ns, flow.identity, *selected, stage, flow.stage == stages_ ? ServingStep::Result : ServingStep::Stage
        };
    }

    // Result assembly changes the pool's graph ports before feedback commits its unique page ledger.
    if (busy.front() || busy.back()) return std::nullopt;
    const auto & admission = groups.at(group_);
    std::optional<ServingAction> candidate;
    for (std::size_t lane = 0; lane < lanes_.size(); ++lane) {
        if (lanes_[lane].flow) continue;
        auto work = plan_batch(lane, now_ns, admission);
        if (!work) continue;
        if (candidate && work->batch.phase == Phase::Decode) continue;
        candidate = ServingAction{
            { group_, next_task_ },
            std::move(work), now_ns, next_flow_, lane, 0, stages_ == 1 ? ServingStep::Batch : ServingStep::Stage
        };
        if (candidate->work->batch.phase == Phase::Prefill) break;
    }
    return candidate;
}

std::shared_ptr<const BatchWork> ServingController::plan_batch(std::size_t lane, std::int64_t now_ns, const AdmissionConstraints & admission) const {
    const auto & active = lanes_[lane].active;

    // Reserve each active request's peak pages; actual ownership grows only with KV residency.
    auto remaining = settings_.max_total_tokens / settings_.page_size;
    for (const auto & member_lane : lanes_) {
        for (const auto identity : member_lane.active) remaining -= requests_[identity].peak_pages;
    }

    auto input_tokens = settings_.max_prefill_tokens;
    const auto occupied = admission.occupied_members.at(lane);
    const auto available = occupied < batch_limit_ ? batch_limit_ - occupied : 0;
    const auto slots = std::min(kv_.free_slots().size(), available);
    std::vector<std::size_t> admitted;

    // Non-chunked admission consumes complete requests until its token budget is exhausted.
    for (auto index = waiting_; index < requests_.size() && admitted.size() < slots && input_tokens > 0; ++index) {
        const auto & state = requests_[index];
        if (state.input.arrival_ns > now_ns || state.peak_pages > remaining) break;

        // Budget units can be padded; the batch itself still carries actual token lengths.
        const auto granularity = admission.prefill_token_granularity;
        const auto budget_tokens = ((state.input.prompt_tokens - 1) / granularity + 1) * granularity;

        admitted.push_back(index);
        remaining -= state.peak_pages;
        input_tokens = std::max<std::int64_t>(0, input_tokens - budget_tokens);
    }
    if (admitted.empty() && active.empty()) return {};

    // Describe ownership changes without mutating the controller until start().
    const bool prefill = !admitted.empty();
    const auto & selected = prefill ? admitted : active;
    Batch batch{ .phase = prefill ? Phase::Prefill : Phase::Decode,
                 .request_capacity = settings_.max_running_requests,
                 .free_pages_before = static_cast<std::int64_t>(kv_.free_pages().size()),
                 .allocated_pages = 0,
                 .members = {} };
    for (std::size_t index = 0; index < selected.size(); ++index) {
        const auto identity = selected[index];
        const auto & state = requests_[identity];
        const auto & resident = kv_.residence(identity);
        const auto slot = prefill ? kv_.free_slots().at(index) : *resident.slot;
        const auto tokens = prefill ? state.input.prompt_tokens : 1;
        BatchMember member{ identity, slot, resident.tokens, tokens, resident.pages.empty() ? std::nullopt : std::optional(resident.pages.back()), {} };

        const auto required = kv_.pages_for(resident.tokens + tokens) - static_cast<std::int64_t>(resident.pages.size());
        for (std::int64_t page = 0; page < required; ++page) {
            member.new_pages.push_back(kv_.free_pages().at(static_cast<std::size_t>(batch.allocated_pages)));
            ++batch.allocated_pages;
        }

        batch.members.push_back(std::move(member));
    }

    auto completion = effects(batch);
    return std::make_shared<BatchWork>(std::move(batch), std::move(completion));
}

void ServingController::start(const ServingAction & action) {
    if (action.task != TaskIdentity{ group_, next_task_ }) throw std::logic_error("action is not the current control plan");
    if (action.scheduled_ns < last_event_ns_) throw std::invalid_argument("cannot submit a plan before the last completion");
    auto & lane = lanes_.at(action.lane);
    if (!lane.flow) {
        const auto & batch = action.work->batch;
        kv_.reserve(batch);

        for (const auto & member : batch.members) {
            if (batch.phase == Phase::Prefill) {
                lane.active.push_back(member.request);
                ++waiting_;
            }
        }

        lane.flow = Flow{ next_flow_++, action.work, 0, std::nullopt };
    }

    lane.flow->running = action.task;
    last_event_ns_ = action.scheduled_ns;
    ++next_task_;
}

BatchEffects ServingController::effects(const Batch & batch) const {
    BatchEffects result;
    for (const auto & member : batch.members) {
        const auto & state = requests_[member.request];
        const auto & resident = kv_.residence(member.request);
        const auto generated = state.generated_tokens + 1;
        const bool finished = generated == state.input.output_tokens;
        std::vector<std::size_t> released;
        if (finished) {
            released = resident.pages;
            released.insert(released.end(), member.new_pages.begin(), member.new_pages.end());
        }

        result.members.push_back({ .request = member.request,
                                   .slot = member.slot,
                                   .generated_tokens = generated,
                                   .resident_tokens = resident.tokens + member.new_tokens,
                                   .finished = finished,
                                   .emits_output = true,
                                   .released_pages = std::move(released) });
    }

    return result;
}

void ServingController::finish(const ServingAction & action, std::span<const std::size_t> returned_pages, std::int64_t completed_ns) {
    auto & lane = lanes_.at(action.lane);
    if (lane.flow->running != action.task) throw std::logic_error("completion does not belong to the in-flight action");
    if (completed_ns < last_event_ns_) throw std::invalid_argument("batch completion precedes submission");
    last_event_ns_ = completed_ns;
    lane.flow->running.reset();
    if (!action.commits_requests()) {
        ++lane.flow->stage;
        return;
    }

    for (const auto & member : action.work->effects.members) {
        auto & state = requests_.at(member.request);
        state.generated_tokens = member.generated_tokens;
        kv_.advance(member.request, member.resident_tokens);
        if (member.finished) kv_.release(member.request);
    }

    // The expansion supplies the native return order; page ownership and slot reuse remain here.
    kv_.return_pages(returned_pages);
    std::erase_if(lane.active, [this](std::size_t identity) { return !kv_.residence(identity).slot; });
    lane.flow.reset();
}

std::optional<std::int64_t> ServingController::next_arrival_ns(std::int64_t now_ns) const {
    if (waiting_ == requests_.size()) return std::nullopt;

    // Cleanup may unblock a waiting request that has already arrived.
    return std::max(now_ns, requests_[waiting_].input.arrival_ns);
}

bool ServingController::done() const {
    return waiting_ == requests_.size() && std::ranges::all_of(lanes_, [](const auto & lane) { return lane.active.empty() && !lane.flow; });
}
std::size_t ServingController::free_pages() const { return kv_.free_pages().size(); }
std::size_t ServingController::request_count() const { return requests_.size(); }
const Request & ServingController::request(std::size_t identity) const { return requests_.at(identity).input; }

} // namespace markov
