#include "markov/controller/disaggregated.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <utility>

namespace markov {

DisaggregatedController::DisaggregatedController(ServingSettings prefill, ServingSettings decode, std::vector<Request> requests, DisaggregatedGroups groups,
                                                 KvPool & prefill_resources, KvPool & decode_resources, BatchReplay * replay)
    : prefill_settings_(prefill),
      decode_settings_(decode),
      groups_(std::move(groups)),
      prefill_(prefill_resources),
      decode_(decode_resources),
      replay_(replay) {
    if (prefill.page_size != decode.page_size) throw std::invalid_argument("PD page transfer requires matching page sizes");
    for (auto & request : requests) {
        if (prefill_.pages_for(request.prompt_tokens) > prefill_settings_.max_total_tokens / prefill_settings_.page_size
            || decode_.pages_for(request.prompt_tokens + (request.output_tokens - 1)) > decode_settings_.max_total_tokens / decode_settings_.page_size)
            throw std::invalid_argument("request cannot fit its P/D KV capacity: " + request.id);
        requests_.push_back({ std::move(request) });
    }
}

bool DisaggregatedController::RequestState::transferred() const {
    return stage == RequestStage::Transferred || stage == RequestStage::Received || stage == RequestStage::Active || stage == RequestStage::Finished;
}

Batch DisaggregatedController::view(std::size_t request, const KvPool & pool) const {
    auto batch = pool.plan(Phase::Prefill);
    const auto & resident = pool.residence(request);
    batch.members.push_back({ request, resident.slot.value(), resident.allocated_tokens, 0, resident.pages.back(), {}, Phase::Prefill });
    return batch;
}

ServingAction DisaggregatedController::action(ServingStep step, Batch batch, BatchEffects effects, std::int64_t now_ns, std::size_t flow) const {
    if (step == ServingStep::Batch) effects = this->effects(batch);
    const auto & group = step == ServingStep::Model || step == ServingStep::Release ? groups_.prefill
                         : step == ServingStep::Transfer                            ? groups_.transfer
                                                                                    : groups_.decode;
    BatchWork work{ std::move(batch), std::move(effects), {} };
    if (step == ServingStep::Model || step == ServingStep::Receive) {

        // Lightweight control has one metadata lease per resident request.
        for (const auto & member : work.batch.members) work.metadata_slots.emplace(member.request, member.slot);
    }
    std::size_t lane = 0;
    if (replay_ && step == ServingStep::Model) lane = replay_->next("prefill")->lane;
    if (replay_ && (step == ServingStep::Batch || step == ServingStep::Accept)) lane = replay_->next("decode")->lane;
    if (step == ServingStep::Transfer) {
        const auto request = work.batch.members.front().request;
        const auto & source = prefill_.residence(request).pages;
        const auto & destination = decode_.residence(request).pages;
        for (std::size_t page = 0; page < source.size(); ++page) work.transferred_pages.push_back({ source[page], destination.at(page) });
        auto & member = work.batch.members.front();
        member.new_tokens = member.context_tokens;
        member.context_tokens = 0;
    }
    return {
        { group, next_task_ },
        std::make_shared<const BatchWork>(std::move(work)), now_ns, flow, lane, std::nullopt, step
    };
}

BatchEffects DisaggregatedController::effects(const Batch & batch) const {
    BatchEffects result;
    for (const auto & member : batch.members) {
        const auto & state = requests_[member.request];
        const auto & resident = decode_.residence(member.request);
        const auto generated = state.generated + 1;
        const bool finished = generated == state.input.output_tokens;
        std::vector<std::size_t> mapped;
        if (finished || member.phase == Phase::Prefill) {
            mapped = resident.pages;
            mapped.insert(mapped.end(), member.new_pages.begin(), member.new_pages.end());
        }
        result.members.push_back({ member.request,
                                   member.slot,
                                   generated,
                                   member.context_tokens + member.new_tokens,
                                   finished,
                                   true,
                                   finished ? mapped : std::vector<std::size_t>{},
                                   false,
                                   std::move(mapped) });
    }
    return result;
}

std::optional<ServingAction> DisaggregatedController::plan(std::int64_t now_ns, const GroupAdmission &) {
    if (now_ns < last_event_ns_) throw std::invalid_argument("control time must not move backwards");

    // Completed transport releases the source independently of destination token processing.
    if (!prefill_busy_) {
        for (const auto request : prefill_active_) {
            const auto & state = requests_[request];
            const auto & resident = prefill_.residence(request);
            if (!state.transferred() || !resident.slot) continue;
            auto batch = view(request, prefill_);
            BatchEffects completion{
                { { request, resident.slot.value(), state.generated, resident.allocated_tokens, true, false, resident.pages, false, resident.pages } }
            };
            return action(ServingStep::Release, std::move(batch), std::move(completion), now_ns, state.flow);
        }
    }

    if (!decode_busy_) {

        // Transport installs the first token before native admission selects a prebuilt batch.
        for (const auto request : active_) {
            const auto & state = requests_[request];
            if (state.stage != RequestStage::Transferred) continue;
            auto batch = view(request, decode_);
            batch.members.front().new_tokens = batch.members.front().context_tokens;
            batch.members.front().context_tokens = 0;
            return action(ServingStep::Receive, std::move(batch), {}, now_ns, state.flow);
        }
    }

    const auto * recorded_decode = replay_ ? replay_->next("decode") : nullptr;
    if (!decode_busy_ && recorded_decode && recorded_decode->prebuilt) {
        auto batch = decode_.plan(Phase::Prefill);
        bool ready = true;
        for (const auto & member : recorded_decode->members) {
            if (requests_[member.request].stage != RequestStage::Received) {
                ready = false;
                break;
            }
            auto installed = view(member.request, decode_).members.front();
            installed.new_tokens = installed.context_tokens;
            installed.context_tokens = 0;
            batch.members.push_back(std::move(installed));
        }
        if (ready) {
            auto completion = effects(batch);
            return action(ServingStep::Accept, std::move(batch), std::move(completion), now_ns, next_task_);
        }
    }
    if (!decode_busy_ && !replay_) {
        for (const auto request : active_) {
            const auto & state = requests_[request];
            if (state.stage != RequestStage::Received) continue;
            auto batch = view(request, decode_);
            batch.members.front().new_tokens = batch.members.front().context_tokens;
            batch.members.front().context_tokens = 0;
            auto completion = effects(batch);
            return action(ServingStep::Accept, std::move(batch), std::move(completion), now_ns, state.flow);
        }
    }

    if (!transfer_busy_) {
        for (const auto request : active_) {
            const auto & state = requests_[request];
            if (state.stage == RequestStage::ModelReady) return action(ServingStep::Transfer, view(request, decode_), {}, now_ns, state.flow);
        }
    }

    const auto waiting = next_waiting();
    if (!decode_busy_ && waiting && !decode_.free_slots().empty()) {
        auto remaining = decode_settings_.max_total_tokens / decode_settings_.page_size;
        for (const auto request : active_) {
            const auto & input = requests_[request].input;
            remaining -= decode_.pages_for(input.prompt_tokens + (input.output_tokens - 1));
        }
        const auto & next = requests_[*waiting].input;
        if (next.arrival_ns <= now_ns && decode_.pages_for(next.prompt_tokens + (next.output_tokens - 1)) <= remaining) {
            const std::array demands{
                KvDemand{ *waiting, next.prompt_tokens }
            };
            auto batch = decode_.plan(Phase::Prefill, demands);
            return action(ServingStep::Reserve, std::move(batch), {}, now_ns, next_task_);
        }
    }

    if (!prefill_busy_ && replay_) {
        const auto * recorded = replay_->next("prefill");
        if (recorded) {
            if (recorded->members.size() > static_cast<std::size_t>(prefill_settings_.max_running_requests))
                throw std::invalid_argument("recorded prefill batch exceeds request capacity");
            const bool ready = recorded->members.size() <= prefill_.free_slots().size() && std::ranges::all_of(recorded->members, [&](const auto & member) {
                                   return requests_[member.request].stage == RequestStage::Reserved;
                               });
            if (ready) {
                std::vector<KvDemand> demands;
                for (const auto & member : recorded->members) demands.push_back({ member.request, member.new_tokens });
                auto batch = prefill_.plan(Phase::Prefill, demands);

                BatchEffects completion;
                for (const auto & member : batch.members) {
                    completion.members.push_back({ member.request, member.slot, 1, member.new_tokens, false, false, {}, false, member.new_pages });
                }
                return action(ServingStep::Model, std::move(batch), std::move(completion), now_ns, next_task_);
            }
        }
    }
    if (!prefill_busy_ && !replay_ && !prefill_.free_slots().empty()) {
        for (const auto request : active_) {
            const auto & state = requests_[request];
            if (state.stage != RequestStage::Reserved
                || prefill_.pages_for(state.input.prompt_tokens) > static_cast<std::int64_t>(prefill_.free_pages().size()))
                continue;
            const std::array demands{
                KvDemand{ request, state.input.prompt_tokens }
            };
            auto batch = prefill_.plan(Phase::Prefill, demands);
            BatchEffects completion{
                { { request, batch.members.front().slot, 1, state.input.prompt_tokens, false, false, {}, false, batch.members.front().new_pages } }
            };
            return action(ServingStep::Model, std::move(batch), std::move(completion), now_ns, state.flow);
        }
    }

    if (!decode_busy_ && (!replay_ || (recorded_decode && !recorded_decode->prebuilt))) {
        std::vector<KvDemand> demands;
        if (recorded_decode) {
            for (const auto & member : recorded_decode->members) {
                if (requests_[member.request].stage != RequestStage::Active) return std::nullopt;
                if (decode_.residence(member.request).allocated_tokens != member.context_tokens)
                    throw std::invalid_argument("recorded decode context differs from allocated KV");
                demands.push_back({ member.request, member.new_tokens });
            }
        }
        else {
            for (const auto request : active_) {
                if (requests_[request].stage == RequestStage::Active) demands.push_back({ request, 1 });
            }
        }
        if (!demands.empty()) return action(ServingStep::Batch, decode_.plan(Phase::Decode, demands), {}, now_ns, next_task_);
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
    case ServingStep::Receive:
    case ServingStep::Accept:
    case ServingStep::Batch:
        return decode_busy_;
    default:
        throw std::logic_error("PD control received a pipeline stage action");
    }
}

void DisaggregatedController::start(const ServingAction & action) {
    if (action.task.sequence != next_task_ || action.scheduled_ns < last_event_ns_) throw std::logic_error("action is not the current PD plan");
    const auto & batch = action.batch_work().batch;
    if (action.step == ServingStep::Reserve) {
        decode_.reserve(batch);
        const auto identity = batch.members.front().request;
        auto & request = requests_[identity];
        request.flow = action.flow;
        active_.push_back(identity);
        ++waiting_;
    }
    else if (action.step == ServingStep::Model) {
        prefill_.reserve(batch);
        for (const auto & member : batch.members) prefill_active_.push_back(member.request);
    }
    else if (action.step == ServingStep::Batch) decode_.reserve(batch);

    if (replay_ && action.step == ServingStep::Model) replay_->advance("prefill");
    if (replay_ && (action.step == ServingStep::Accept || action.step == ServingStep::Batch)) replay_->advance("decode");

    channel(action.step) = action.task;
    last_event_ns_ = action.scheduled_ns;
    ++next_task_;
}

void DisaggregatedController::finish(const ServingAction & action, const BatchProgram & program, std::int64_t completed_ns) {
    const auto & returned_pages = program.returned_pages;
    auto & busy = channel(action.step);
    if (busy != action.task || completed_ns < last_event_ns_) throw std::logic_error("completion does not belong to the pending PD action");
    busy.reset();
    last_event_ns_ = completed_ns;
    const auto request = action.batch_work().batch.members.front().request;
    auto & state = requests_[request];

    switch (action.step) {
    case ServingStep::Reserve:
        state.stage = RequestStage::Reserved;
        break;
    case ServingStep::Model:
        for (const auto & member : action.batch_work().batch.members) { requests_[member.request].stage = RequestStage::ModelReady; }
        break;
    case ServingStep::Transfer:
        state.stage = RequestStage::Transferred;
        break;
    case ServingStep::Receive:
        state.stage = RequestStage::Received;
        break;
    case ServingStep::Release:
        prefill_.release(request);
        prefill_.return_pages(returned_pages);
        std::erase(prefill_active_, request);
        break;
    case ServingStep::Accept:
    case ServingStep::Batch:
        for (const auto & member : action.batch_work().effects.members) {
            auto & current = requests_[member.request];
            current.generated = member.generated_tokens;
            current.stage = member.finished ? RequestStage::Finished : RequestStage::Active;
            if (member.finished) decode_.release(member.request);
        }
        decode_.return_pages(returned_pages);
        std::erase_if(active_, [this](std::size_t identity) { return requests_[identity].stage == RequestStage::Finished; });
        break;
    default:
        throw std::logic_error("PD control received a pipeline stage action");
    }
}

std::optional<std::size_t> DisaggregatedController::next_waiting() const {
    if (!replay_) return waiting_ < requests_.size() ? std::optional(waiting_) : std::nullopt;

    // Reserve the upcoming native batch, even when its request order differs from input order.
    if (const auto * batch = replay_->next("prefill")) {
        for (const auto & member : batch->members) {
            if (requests_[member.request].stage == RequestStage::Waiting) return member.request;
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> DisaggregatedController::next_arrival_ns(std::int64_t now_ns) const {
    const auto waiting = next_waiting();
    if (waiting && requests_[*waiting].input.arrival_ns > now_ns) return requests_[*waiting].input.arrival_ns;
    return std::nullopt;
}

bool DisaggregatedController::done() const {
    return waiting_ == requests_.size() && active_.empty() && prefill_active_.empty() && !prefill_busy_ && !decode_busy_ && !transfer_busy_;
}

std::size_t DisaggregatedController::free_pages() const { return decode_.free_pages().size(); }
std::size_t DisaggregatedController::request_count() const { return requests_.size(); }
const Request & DisaggregatedController::request(std::size_t identity) const { return requests_.at(identity).input; }

} // namespace markov
