#include "markov/controller/disaggregated.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>

namespace markov {

DisaggregatedController::DisaggregatedController(ServingSettings prefill, ServingSettings decode, std::vector<Request> requests, DisaggregatedGroups groups,
                                                 PageReuse reuse)
    : prefill_settings_(prefill),
      decode_settings_(decode),
      groups_(std::move(groups)),
      prefill_(requests.size(), prefill, reuse),
      decode_(requests.size(), decode, reuse) {
    if (prefill.page_size != decode.page_size) throw std::invalid_argument("PD page transfer requires matching page sizes");
    for (auto & request : Request::ordered(std::move(requests))) {
        if (prefill_.pages_for(request.prompt_tokens) > prefill_settings_.max_total_tokens / prefill_settings_.page_size
            || decode_.pages_for(request.prompt_tokens + (request.output_tokens - 1)) > decode_settings_.max_total_tokens / decode_settings_.page_size)
            throw std::invalid_argument("request cannot fit its P/D KV capacity: " + request.id);
        requests_.push_back({ std::move(request) });
    }
}

bool DisaggregatedController::RequestState::transferred() const {
    return stage == RequestStage::Transferred || stage == RequestStage::Active || stage == RequestStage::Finished;
}

Batch DisaggregatedController::new_batch(const KvPool & pool, const ServingSettings & settings, Phase phase) const {
    return { phase, settings.max_running_requests, static_cast<std::int64_t>(pool.free_pages().size()), 0, {}, pool.deferred_pages() };
}

void DisaggregatedController::append_member(Batch & batch, const KvPool & pool, std::size_t request, std::int64_t tokens) const {
    const auto & resident = pool.residence(request);
    const auto slot = resident.slot ? *resident.slot : pool.free_slots().front();
    BatchMember member{ request, slot, resident.tokens, tokens, resident.pages.empty() ? std::nullopt : std::optional(resident.pages.back()), {} };
    const auto pages = pool.pages_for(resident.tokens + tokens) - static_cast<std::int64_t>(resident.pages.size());
    for (std::int64_t index = 0; index < pages; ++index)
        member.new_pages.push_back(pool.free_pages().at(static_cast<std::size_t>(batch.allocated_pages + index)));
    batch.allocated_pages += pages;
    batch.members.push_back(std::move(member));
}

Batch DisaggregatedController::view(std::size_t request, const KvPool & pool, const ServingSettings & settings) const {
    auto batch = new_batch(pool, settings, Phase::Prefill);
    const auto & resident = pool.residence(request);
    batch.members.push_back({ request, resident.slot.value(), resident.tokens, 0, resident.pages.back(), {} });
    return batch;
}

ServingAction DisaggregatedController::action(ServingStep step, Batch batch, BatchEffects effects, std::int64_t now_ns, std::size_t flow) const {
    if (step == ServingStep::Model) prefill_.assign_pages(batch);
    else if (step == ServingStep::Reserve || step == ServingStep::Batch) decode_.assign_pages(batch);
    if (step == ServingStep::Batch) effects = this->effects(batch, false);
    const auto & group = step == ServingStep::Model || step == ServingStep::Release ? groups_.prefill
                         : step == ServingStep::Transfer                            ? groups_.transfer
                                                                                    : groups_.decode;
    BatchWork work{ std::move(batch), std::move(effects), {} };
    if (step == ServingStep::Transfer) {
        const auto request = work.batch.members.front().request;
        const auto & source = prefill_.residence(request).pages;
        const auto & destination = decode_.residence(request).pages;
        for (std::size_t page = 0; page < source.size(); ++page) work.transferred_pages.push_back({ source[page], destination.at(page) });
    }
    return {
        { group, next_task_ },
        std::make_shared<const BatchWork>(std::move(work)), now_ns, flow, 0, std::nullopt, step
    };
}

BatchEffects DisaggregatedController::effects(const Batch & batch, bool first_output) const {
    BatchEffects result;
    for (const auto & member : batch.members) {
        const auto & state = requests_[member.request];
        const auto & resident = decode_.residence(member.request);
        const auto generated = state.generated + 1;
        const bool finished = generated == state.input.output_tokens;
        auto pages = finished ? resident.pages : std::vector<std::size_t>{};
        if (finished) pages.insert(pages.end(), member.new_pages.begin(), member.new_pages.end());
        result.members.push_back(
            { member.request, member.slot, generated, resident.tokens + (first_output ? 0 : member.new_tokens), finished, true, std::move(pages) });
    }
    return result;
}

std::optional<ServingAction> DisaggregatedController::plan(std::int64_t now_ns, const GroupAdmission &) const {
    if (now_ns < last_event_ns_) throw std::invalid_argument("control time must not move backwards");

    // Completed transport releases the source independently of destination token processing.
    if (!prefill_busy_) {
        for (const auto request : prefill_active_) {
            const auto & state = requests_[request];
            const auto & resident = prefill_.residence(request);
            if (!state.transferred() || !resident.slot) continue;
            auto batch = view(request, prefill_, prefill_settings_);
            BatchEffects completion{ { { request, resident.slot.value(), state.generated, resident.tokens, true, false, resident.pages } } };
            return action(ServingStep::Release, std::move(batch), std::move(completion), now_ns, state.flow);
        }
    }

    if (!decode_busy_) {
        for (const auto request : active_) {
            const auto & state = requests_[request];
            if (state.stage != RequestStage::Transferred) continue;
            auto batch = view(request, decode_, decode_settings_);
            batch.members.front().new_tokens = batch.members.front().context_tokens;
            batch.members.front().context_tokens = 0;
            auto completion = effects(batch, true);
            return action(ServingStep::Accept, std::move(batch), std::move(completion), now_ns, state.flow);
        }
    }

    if (!transfer_busy_) {
        for (const auto request : active_) {
            const auto & state = requests_[request];
            if (state.stage == RequestStage::ModelReady) return action(ServingStep::Transfer, view(request, decode_, decode_settings_), {}, now_ns, state.flow);
        }
    }

    if (!decode_busy_ && waiting_ < requests_.size() && !decode_.free_slots().empty()) {
        auto remaining = decode_settings_.max_total_tokens / decode_settings_.page_size;
        for (const auto request : active_) {
            const auto & input = requests_[request].input;
            remaining -= decode_.pages_for(input.prompt_tokens + (input.output_tokens - 1));
        }
        const auto & next = requests_[waiting_].input;
        if (next.arrival_ns <= now_ns && decode_.pages_for(next.prompt_tokens + (next.output_tokens - 1)) <= remaining) {
            auto batch = new_batch(decode_, decode_settings_, Phase::Prefill);
            append_member(batch, decode_, waiting_, next.prompt_tokens);
            return action(ServingStep::Reserve, std::move(batch), {}, now_ns, next_task_);
        }
    }

    if (!prefill_busy_ && !prefill_.free_slots().empty()) {
        for (const auto request : active_) {
            const auto & state = requests_[request];
            if (state.stage != RequestStage::Reserved
                || prefill_.pages_for(state.input.prompt_tokens) > static_cast<std::int64_t>(prefill_.free_pages().size()))
                continue;
            auto batch = new_batch(prefill_, prefill_settings_, Phase::Prefill);
            append_member(batch, prefill_, request, state.input.prompt_tokens);
            BatchEffects completion{ { { request, batch.members.front().slot, 1, state.input.prompt_tokens, false, false, {} } } };
            return action(ServingStep::Model, std::move(batch), std::move(completion), now_ns, state.flow);
        }
    }

    if (!decode_busy_) {
        auto batch = new_batch(decode_, decode_settings_, Phase::Decode);
        for (const auto request : active_) {
            if (requests_[request].stage == RequestStage::Active) append_member(batch, decode_, request, 1);
        }
        if (!batch.members.empty()) { return action(ServingStep::Batch, std::move(batch), {}, now_ns, next_task_); }
    }
    return std::nullopt;
}

std::optional<TaskIdentity> & DisaggregatedController::channel(ServingStep step) {
    switch (step) {
    case ServingStep::Model:
    case ServingStep::Release:
        return prefill_busy_;
    case ServingStep::Transfer:
        return transfer_busy_;
    case ServingStep::Reserve:
    case ServingStep::Accept:
    case ServingStep::Batch:
        return decode_busy_;
    default:
        throw std::logic_error("PD control received a pipeline stage action");
    }
}

void DisaggregatedController::start(const ServingAction & action) {
    if (action.task.sequence != next_task_ || action.scheduled_ns < last_event_ns_) throw std::logic_error("action is not the current PD plan");
    const auto & batch = action.work->batch;
    if (action.step == ServingStep::Reserve) {
        decode_.reserve(batch);
        auto & request = requests_[waiting_];
        request.flow = action.flow;
        active_.push_back(waiting_++);
    }
    else if (action.step == ServingStep::Model) {
        prefill_.reserve(batch);
        prefill_active_.push_back(batch.members.front().request);
    }
    else if (action.step == ServingStep::Batch) decode_.reserve(batch);

    channel(action.step) = action.task;
    last_event_ns_ = action.scheduled_ns;
    ++next_task_;
}

void DisaggregatedController::finish(const ServingAction & action, std::span<const std::size_t> returned_pages, std::int64_t completed_ns) {
    auto & busy = channel(action.step);
    if (busy != action.task || completed_ns < last_event_ns_) throw std::logic_error("completion does not belong to the pending PD action");
    busy.reset();
    last_event_ns_ = completed_ns;
    const auto request = action.work->batch.members.front().request;
    auto & state = requests_[request];

    switch (action.step) {
    case ServingStep::Reserve:
        decode_.advance(request, state.input.prompt_tokens);
        state.stage = RequestStage::Reserved;
        break;
    case ServingStep::Model:
        prefill_.advance(request, state.input.prompt_tokens);
        state.stage = RequestStage::ModelReady;
        break;
    case ServingStep::Transfer:
        state.stage = RequestStage::Transferred;
        break;
    case ServingStep::Release:
        prefill_.release(request);
        prefill_.return_pages(returned_pages);
        std::erase(prefill_active_, request);
        break;
    case ServingStep::Accept:
    case ServingStep::Batch:
        for (const auto & member : action.work->effects.members) {
            auto & current = requests_[member.request];
            current.generated = member.generated_tokens;
            current.stage = member.finished ? RequestStage::Finished : RequestStage::Active;
            decode_.advance(member.request, member.resident_tokens);
            if (member.finished) decode_.release(member.request);
        }
        decode_.return_pages(returned_pages);
        std::erase_if(active_, [this](std::size_t identity) { return requests_[identity].stage == RequestStage::Finished; });
        break;
    default:
        throw std::logic_error("PD control received a pipeline stage action");
    }
}

std::optional<std::int64_t> DisaggregatedController::next_arrival_ns(std::int64_t now_ns) const {
    if (waiting_ < requests_.size() && requests_[waiting_].input.arrival_ns > now_ns) return requests_[waiting_].input.arrival_ns;
    return std::nullopt;
}

bool DisaggregatedController::done() const {
    return waiting_ == requests_.size() && active_.empty() && prefill_active_.empty() && !prefill_busy_ && !decode_busy_ && !transfer_busy_;
}

std::size_t DisaggregatedController::free_pages() const { return decode_.free_pages().size(); }
std::size_t DisaggregatedController::request_count() const { return requests_.size(); }
const Request & DisaggregatedController::request(std::size_t identity) const { return requests_.at(identity).input; }

std::map<std::string, std::size_t> DisaggregatedController::pool_free_pages() const {
    return {
        { groups_.prefill, prefill_.free_pages().size() },
        {  groups_.decode,  decode_.free_pages().size() }
    };
}

} // namespace markov
