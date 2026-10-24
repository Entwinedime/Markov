#include "markov/controller/serving.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov {

ServingController::ServingController(ServingSettings settings, std::vector<Request> requests) : settings_(settings) {
    const auto capacity = settings.max_total_tokens / settings.page_size;
    std::set<std::string> identities;
    std::stable_sort(requests.begin(), requests.end(), [](const Request & left, const Request & right) { return left.arrival_ns < right.arrival_ns; });

    for (auto & request : requests) {
        if (request.id.empty() || !identities.insert(request.id).second) throw std::invalid_argument("request IDs must be nonempty and unique");
        if (request.arrival_ns < 0 || request.prompt_tokens <= 0 || request.output_tokens <= 0)
            throw std::invalid_argument("request needs nonnegative arrival and positive token lengths: " + request.id);
        if (request.output_tokens - 1 > std::numeric_limits<std::int64_t>::max() - request.prompt_tokens)
            throw std::invalid_argument("request token length overflows: " + request.id);

        // Prefill emits the first output; only later outputs append that previous token to KV.
        const auto peak = pages_for(request.prompt_tokens + (request.output_tokens - 1));
        if (peak > capacity) throw std::invalid_argument("request cannot fit the KV capacity: " + request.id);
        requests_.push_back({ std::move(request), peak, std::nullopt, 0, 0, {} });
    }

    free_slots_.resize(static_cast<std::size_t>(settings.max_running_requests));
    free_pages_.resize(static_cast<std::size_t>(capacity));
    std::iota(free_slots_.begin(), free_slots_.end(), std::size_t{ 0 });
    std::iota(free_pages_.begin(), free_pages_.end(), std::size_t{ 0 });
}

std::int64_t ServingController::pages_for(std::int64_t tokens) const { return tokens == 0 ? 0 : (tokens - 1) / settings_.page_size + 1; }

std::optional<Batch> ServingController::plan(std::int64_t now_ns, AdmissionConstraints admission) const {
    if (now_ns < last_event_ns_) throw std::invalid_argument("control time must not move backwards");
    if (in_flight_) return std::nullopt;

    // Reserve each active request's peak pages; actual ownership grows only with KV residency.
    auto remaining = settings_.max_total_tokens / settings_.page_size;
    for (const auto identity : active_) remaining -= requests_[identity].peak_pages;

    auto input_tokens = settings_.max_prefill_tokens;
    const auto capacity = static_cast<std::size_t>(settings_.max_running_requests);
    const auto available = admission.occupied_members < capacity ? capacity - admission.occupied_members : 0;
    const auto slots = std::min(free_slots_.size(), available);
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
    if (admitted.empty() && active_.empty()) return std::nullopt;

    // Describe ownership changes without mutating the controller until start().
    const bool prefill = !admitted.empty();
    const auto & selected = prefill ? admitted : active_;
    Batch batch{ prefill ? Phase::Prefill : Phase::Decode, now_ns, static_cast<std::int64_t>(free_pages_.size()), 0, {} };
    for (std::size_t index = 0; index < selected.size(); ++index) {
        const auto identity = selected[index];
        const auto & state = requests_[identity];
        const auto slot = prefill ? free_slots_.at(index) : *state.slot;
        const auto tokens = prefill ? state.input.prompt_tokens : 1;
        BatchMember member{ identity, slot, state.resident_tokens, tokens, state.pages.empty() ? std::nullopt : std::optional(state.pages.back()), {} };

        const auto required = pages_for(state.resident_tokens + tokens) - static_cast<std::int64_t>(state.pages.size());
        for (std::int64_t page = 0; page < required; ++page) {
            member.new_pages.push_back(free_pages_.at(static_cast<std::size_t>(batch.allocated_pages)));
            ++batch.allocated_pages;
        }

        batch.members.push_back(std::move(member));
    }

    return batch;
}

void ServingController::start(Batch batch) {
    if (in_flight_) throw std::logic_error("only one model batch may be in flight");
    if (batch.scheduled_ns < last_event_ns_) throw std::invalid_argument("cannot submit a plan before the last completion");

    for (const auto & member : batch.members) {
        auto & state = requests_.at(member.request);
        if (batch.phase == Phase::Prefill) {
            state.slot = member.slot;
            free_slots_.pop_front();
            active_.push_back(member.request);
            ++waiting_;
        }

        state.pages.insert(state.pages.end(), member.new_pages.begin(), member.new_pages.end());
    }

    free_pages_.erase(free_pages_.begin(), free_pages_.begin() + batch.allocated_pages);
    last_event_ns_ = batch.scheduled_ns;
    in_flight_ = std::move(batch);
}

BatchEffects ServingController::effects(const Batch & batch) const {
    BatchEffects result;
    for (const auto & member : batch.members) {
        const auto & state = requests_[member.request];
        const auto generated = state.generated_tokens + 1;
        const bool finished = generated == state.input.output_tokens;
        std::vector<std::size_t> released;
        if (finished) {
            released = state.pages;
            released.insert(released.end(), member.new_pages.begin(), member.new_pages.end());
        }

        result.members.push_back({ member.request, member.slot, generated, state.resident_tokens + member.new_tokens, finished, std::move(released) });
    }

    return result;
}

void ServingController::finish(const BatchEffects & effects, std::span<const std::size_t> returned_pages, std::int64_t completed_ns) {
    if (!in_flight_) throw std::logic_error("no model batch is in flight");
    if (completed_ns < last_event_ns_) throw std::invalid_argument("batch completion precedes submission");

    for (const auto & member : effects.members) {
        auto & state = requests_.at(member.request);
        state.resident_tokens = member.resident_tokens;
        state.generated_tokens = member.generated_tokens;

        if (member.finished) {
            free_slots_.push_back(member.slot);
            state.slot.reset();
            state.pages = std::vector<std::size_t>{};
        }
    }

    // The expansion supplies the native return order; page ownership and slot reuse remain here.
    free_pages_.insert(free_pages_.begin(), returned_pages.begin(), returned_pages.end());
    std::erase_if(active_, [this](std::size_t identity) { return !requests_[identity].slot; });

    in_flight_.reset();
    last_event_ns_ = completed_ns;
}

std::optional<std::int64_t> ServingController::next_arrival_ns(std::int64_t now_ns) const {
    if (waiting_ == requests_.size()) return std::nullopt;

    // Cleanup may unblock a waiting request that has already arrived.
    return std::max(now_ns, requests_[waiting_].input.arrival_ns);
}

bool ServingController::done() const { return waiting_ == requests_.size() && active_.empty() && !in_flight_; }
std::size_t ServingController::free_pages() const { return free_pages_.size(); }
std::size_t ServingController::request_count() const { return requests_.size(); }
const Request & ServingController::request(std::size_t identity) const { return requests_.at(identity).input; }

} // namespace markov
