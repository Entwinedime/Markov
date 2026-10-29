#include "markov/controller/native_group.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>

namespace markov {

NativeGroup::NativeGroup(const std::vector<Request> & requests, KvPool & resources, std::string group, std::size_t stage, NativeRole role)
    : requests_(requests),
      resources_(resources),
      group_(std::move(group)),
      stage_(stage),
      role_(role) {}

void NativeGroup::begin_iteration(std::size_t flow, std::size_t lane) {
    if (const auto previous = lanes_.find(lane); previous != lanes_.end()) {
        if (const auto frame = frames_.find(previous->second); frame != frames_.end() && frame->second.next != ServingStep::Batch)
            throw std::invalid_argument("native lane reused before its result completed");
    }

    // Output forwarding retains the old snapshot until its native transport user releases it.
    lanes_.insert_or_assign(lane, flow);
}

std::optional<ServingAction> NativeGroup::plan(const nlohmann::json & message, std::int64_t now_ns, std::size_t delivered) {
    const auto kind = message.at("kind").get<std::string>();
    if (kind == "declare_slots" || kind == "capacity" || kind == "free_slots" || kind == "slots" || kind == "allocate" || kind == "reserve_pages"
        || kind == "prebuilt" || kind == "release_pages" || kind == "release_work")
        return std::nullopt;
    const auto flow = message.at("flow").get<std::size_t>();
    const auto lane = message.at("lane").get<std::size_t>();
    if (kind == "reserve" || kind == "received") {
        const auto request = message.at("request").get<std::size_t>();
        std::shared_ptr<const BatchWork> work;
        if (kind == "reserve") work = reservations_.at(request);
        else {
            const auto slot = message.at("metadata_slot").get<std::int64_t>();
            if (slot < 0) throw std::invalid_argument("native receive requires an allocated metadata slot");
            auto batch = resident_batch(nlohmann::json::array({ request }), delivered);
            batch.phase = Phase::Prefill;
            batch.members.front().new_tokens = batch.members.front().context_tokens;
            batch.members.front().context_tokens = 0;
            auto received = std::make_shared<BatchWork>(std::move(batch), BatchEffects{});
            received->metadata_slots.emplace(request, static_cast<std::size_t>(slot));
            work = std::move(received);
        }
        return ServingAction{
            { group_, sequence_ },
            std::move(work), now_ns, flow, lane, stage_, kind == "reserve" ? ServingStep::Reserve : ServingStep::Receive
        };
    }
    const auto frame = frames_.find(flow);
    const bool preparing = frame == frames_.end() || frame->second.next == ServingStep::Prepare;
    if (kind == "select" || (kind == "release_request" && (preparing || role_ == NativeRole::Prefill))) {
        if (kind == "select" && !preparing) throw std::invalid_argument("native selection is outside batch preparation");
        auto batch = resident_batch(kind == "select" ? message.at("requests") : nlohmann::json::array({ message.at("request") }), delivered);
        BatchEffects effects;
        if (kind == "release_request") {
            const auto & member = batch.members.front();
            effects.members.push_back({ member.request,
                                        member.slot,
                                        member.context_tokens - requests_.at(member.request).prompt_tokens + 1,
                                        member.context_tokens,
                                        true,
                                        false,
                                        resources_.residence(member.request).pages });
        }
        return ServingAction{
            { group_, sequence_ },
            std::make_shared<BatchWork>(std::move(batch), std::move(effects)),
            now_ns,
            flow,
            lane,
            stage_,
            kind == "select" ? ServingStep::Select : ServingStep::Release
        };
    }
    if (kind == "release_request") return std::nullopt;
    static const std::map<std::string, ServingStep> steps{
        {     "prepare",    ServingStep::Prepare },
        {       "model",    ServingStep::Compute },
        {        "send",       ServingStep::Send },
        {     "receive",    ServingStep::Receive },
        {        "wait",       ServingStep::Wait },
        { "read_result", ServingStep::ReadResult },
        {      "result",     ServingStep::Result },
        {      "accept",     ServingStep::Accept }
    };
    const auto step = steps.at(kind);
    auto & current = frames_.at(flow);
    const bool message_step = step == ServingStep::Send || step == ServingStep::Receive || step == ServingStep::Wait;
    if (lane != current.lane || (!message_step && step != current.next))
        throw std::invalid_argument("native work does not follow its own preparation/model/read completion");
    auto work = current.work;
    if (step == ServingStep::Result || step == ServingStep::Accept) {
        auto result = std::make_shared<BatchWork>(current.work->batch, result_effects(current, message.at("members")));
        if (role_ == NativeRole::Prefill) {
            for (const auto & member : message.at("members")) {
                const auto slot = member.at("metadata_slot").get<std::int64_t>();
                if (slot < 0) throw std::invalid_argument("native handoff requires an allocated metadata slot");
                result->metadata_slots.emplace(member.at("request").get<std::size_t>(), static_cast<std::size_t>(slot));
            }
        }
        work = std::move(result);
    }
    return ServingAction{
        { group_, sequence_ },
        std::move(work), now_ns, flow, lane, stage_, step, message_step ? message.at("channel").get<std::string>() : std::string{}
    };
}

nlohmann::json NativeGroup::resource_call(const nlohmann::json & message, std::size_t delivered) {
    const auto kind = message.at("kind").get<std::string>();
    if (kind == "declare_slots") {
        resources_.declare_slots(message.at("slots").get<std::size_t>());
        return nullptr;
    }
    if (kind == "free_slots") return resources_.free_slots();
    if (kind == "capacity")
        return {
            { "slots", resources_.free_slots().size() },
            { "pages", resources_.free_pages().size() }
        };
    if (kind == "slots" || kind == "allocate" || kind == "reserve_pages") {
        if (kind == "allocate" && frames_.contains(message.at("flow").get<std::size_t>()))
            throw std::invalid_argument("native allocation replaced a live work snapshot");
        std::vector<KvDemand> demands;
        std::set<std::size_t> members;
        for (const auto & member : message.at("members")) {
            const auto identity = member.at("request").get<std::size_t>();
            if (identity >= delivered || !members.insert(identity).second)
                throw std::invalid_argument("native allocation has an undelivered or repeated request");
            const auto tokens = kind == "slots" ? 0 : member.at("tokens").get<std::int64_t>();
            if (tokens < 0) throw std::invalid_argument("native allocation has negative token demand");
            demands.push_back({ identity, tokens });
        }
        const auto name = kind == "slots" ? "prefill" : message.at("phase").get<std::string>();
        if (name != "prefill" && name != "decode") throw std::invalid_argument("unknown native allocation phase: " + name);
        const auto phase = name == "prefill" ? Phase::Prefill : Phase::Decode;
        if (kind == "reserve_pages" && (demands.size() != 1 || reservations_.contains(demands.front().request)))
            throw std::invalid_argument("native reservation requires one new request");
        auto allocation = resources_.plan(phase, demands);
        resources_.reserve(allocation);
        auto bindings = nlohmann::json::array();
        for (const auto & member : allocation.members) {
            bindings.push_back({
                { "slot", member.slot },
                { "indices", resources_.indices(member.request, member.context_tokens, member.context_tokens + member.new_tokens) }
            });
        }
        if (kind == "allocate") {
            const auto flow = message.at("flow").get<std::size_t>();
            const auto lane = message.at("lane").get<std::size_t>();
            frames_.emplace(flow, Frame{ std::make_shared<BatchWork>(std::move(allocation), BatchEffects{}), lane, ServingStep::Prepare, {}, {}, {} });
        }
        if (kind == "reserve_pages") {
            const auto request = allocation.members.front().request;
            reservations_.emplace(request, std::make_shared<const BatchWork>(std::move(allocation), BatchEffects{}));
        }
        return bindings;
    }
    if (kind == "prebuilt") {
        auto batch = resident_batch(message.at("requests"), delivered);
        batch.phase = Phase::Prefill;
        for (auto & member : batch.members) {
            member.new_tokens = member.context_tokens;
            member.context_tokens = 0;
        }
        const auto flow = message.at("flow").get<std::size_t>();
        if (!frames_
                 .emplace(flow,
                          Frame{ std::make_shared<BatchWork>(std::move(batch), BatchEffects{}),
                                 message.at("lane").get<std::size_t>(),
                                 ServingStep::Accept,
                                 {},
                                 {},
                                 {} })
                 .second)
            throw std::invalid_argument("native prebuilt replaced unfinished work");
        return nullptr;
    }
    const auto found = frames_.find(message.at("flow").get<std::size_t>());
    if (kind == "release_work") {
        auto & frame = frames_.at(message.at("flow").get<std::size_t>());
        frame.released = true;
        if (frame.next == ServingStep::Batch && frame.messages.empty()) frames_.erase(found);
        return nullptr;
    }

    // Prefill handoff release can occur while another lane's model is still in flight.
    Frame * result =
        role_ != NativeRole::Prefill && found != frames_.end() && (found->second.next == ServingStep::Result || found->second.next == ServingStep::Accept)
            ? &found->second
            : nullptr;
    if (kind == "release_request") {
        const auto identity = message.at("request").get<std::size_t>();
        if (result == nullptr || !resources_.residence(identity).slot || !result->released_requests.insert(identity).second)
            throw std::invalid_argument("native slot release is not an active result effect");
        return nullptr;
    }
    if (kind == "release_pages") {
        if (role_ != NativeRole::Prefill && found != frames_.end() && result == nullptr && found->second.next != ServingStep::Prepare)
            throw std::invalid_argument("native page release is outside preparation or result processing");
        const auto pages = message.at("pages").get<std::vector<std::size_t>>();
        auto & released = result == nullptr ? released_pages_ : result->released_pages;
        const auto position = result == nullptr || result->work->batch.phase == Phase::Prefill ? released.begin() : released.end();
        released.insert(position, pages.begin(), pages.end());
        return nullptr;
    }
    throw std::invalid_argument("unknown native control message: " + kind);
}

Batch NativeGroup::resident_batch(const nlohmann::json & requests, std::size_t delivered) const {
    std::vector<KvDemand> demands;
    std::set<std::size_t> members;
    for (const auto & value : requests) {
        const auto identity = value.get<std::size_t>();
        if (identity >= delivered || !members.insert(identity).second || !resources_.residence(identity).slot)
            throw std::invalid_argument("native selection requires unique resident requests");
        demands.push_back({ identity, 0 });
    }
    return resources_.plan(Phase::Decode, demands);
}

BatchEffects NativeGroup::result_effects(const Frame & frame, const nlohmann::json & members) const {
    const auto & batch = frame.work->batch;
    if (members.size() != batch.members.size()) throw std::invalid_argument("native result membership differs from submitted work");
    BatchEffects effects;
    for (std::size_t index = 0; index < members.size(); ++index) {
        const auto & input = members.at(index);
        const auto & member = batch.members[index];
        const auto generated = input.at("generated_tokens").get<std::int64_t>();
        const bool finished = input.at("finished").get<bool>();
        const auto & request = requests_.at(member.request);
        const auto expected = member.context_tokens + member.new_tokens - request.prompt_tokens + 1;
        if (input.at("request") != member.request || generated <= 0 || generated > request.output_tokens || generated != expected
            || finished != (role_ != NativeRole::Prefill && generated == request.output_tokens) || finished != frame.released_requests.contains(member.request))
            throw std::invalid_argument("native result does not match request progress or resource release");
        effects.members.push_back({ member.request,
                                    member.slot,
                                    generated,
                                    member.context_tokens + member.new_tokens,
                                    finished,
                                    input.at("emits_output").get<bool>(),
                                    finished ? resources_.residence(member.request).pages : std::vector<std::size_t>{} });
    }
    if (frame.released_requests.size() != static_cast<std::size_t>(std::ranges::count_if(effects.members, &RequestCompletion::finished)))
        throw std::invalid_argument("native released a request outside the result batch");
    return effects;
}

void NativeGroup::start(const ServingAction & action) {
    if (running_ || action.task != TaskIdentity{ group_, sequence_ }) throw std::logic_error("native submission is not the pending work");
    running_ = action.task;
    ++sequence_;
}

void NativeGroup::finish(const ServingAction & action, std::span<const std::size_t> returned_pages) {
    if (running_ != action.task) throw std::logic_error("native completion does not belong to the running work");
    running_.reset();
    if (action.step == ServingStep::Select) return;
    if (action.step == ServingStep::Receive) return;
    if (action.step == ServingStep::Reserve) {
        const auto & member = action.work->batch.members.front();
        resources_.advance(member.request, member.context_tokens + member.new_tokens);
        reservations_.erase(member.request);
        return;
    }
    if (action.step == ServingStep::Release) {
        if (!std::ranges::equal(returned_pages, released_pages_)) throw std::invalid_argument("native retraction differs from release graph effects");
        resources_.release(action.work->batch.members.front().request);
        resources_.return_pages(returned_pages);
        released_pages_.clear();
        if (role_ == NativeRole::Prefill) ++completed_;
        return;
    }
    auto & frame = frames_.at(action.flow);
    if (action.step == ServingStep::Prepare) frame.next = ServingStep::Compute;
    else if (action.step == ServingStep::Compute) frame.next = ServingStep::ReadResult;
    else if (action.step == ServingStep::ReadResult) frame.next = ServingStep::Result;
    else if (action.step == ServingStep::Send) {
        if (!frame.messages.insert(action.channel).second) throw std::invalid_argument("native send replaced an unconsumed work handle");
    }
    else if (action.step == ServingStep::Wait) {
        if (frame.messages.erase(action.channel) != 1) throw std::invalid_argument("native wait has no matching send");
    }
    else if (action.step == ServingStep::Result || action.step == ServingStep::Accept) {

        // Result effects belong to this work, even while another lane has already allocated or submitted.
        if (!std::ranges::equal(returned_pages, frame.released_pages)) throw std::invalid_argument("native releases differ from result graph effects");
        for (const auto & member : action.work->effects.members) {
            resources_.advance(member.request, member.resident_tokens);
            if (member.finished) {
                resources_.release(member.request);
                ++completed_;
            }
        }
        resources_.return_pages(returned_pages);
        frame.released_requests.clear();
        frame.released_pages.clear();
        frame.next = ServingStep::Batch;
    }
    if (frame.released && frame.next == ServingStep::Batch && frame.messages.empty()) frames_.erase(action.flow);
}

bool NativeGroup::caught_up(std::size_t delivered) const { return completed_ == delivered && !running_; }
bool NativeGroup::done() const { return caught_up(requests_.size()); }
bool NativeGroup::settled() const { return done() && frames_.empty() && reservations_.empty(); }
std::size_t NativeGroup::free_pages() const { return resources_.free_pages().size(); }

} // namespace markov
