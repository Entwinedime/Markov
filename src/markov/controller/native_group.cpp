#include "markov/controller/native_group.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>

namespace markov {

NativeGroup::NativeGroup(const std::vector<Request> & requests, KvPool & resources, std::string group)
    : requests_(requests),
      resources_(resources),
      group_(std::move(group)) {}

void NativeGroup::begin_iteration() { next_ = ServingStep::Stage; }
void NativeGroup::end_iteration() { next_ = ServingStep::Batch; }

std::optional<ServingAction> NativeGroup::plan(const nlohmann::json & message, std::int64_t now_ns, std::size_t delivered) {
    const auto kind = message.at("kind").get<std::string>();
    if (kind == "select" || (kind == "release_request" && next_ == ServingStep::Stage)) {
        if (next_ != ServingStep::Stage) throw std::invalid_argument("native selection is outside batch preparation");
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
            std::make_shared<BatchWork>(std::move(batch), std::move(effects)), now_ns, flow_, 0,
            std::nullopt,          kind == "select" ? ServingStep::Select : ServingStep::Release
        };
    }
    if (kind == "model" || kind == "read_result" || kind == "result") {
        const auto step = kind == "model" ? ServingStep::Stage : kind == "read_result" ? ServingStep::ReadResult : ServingStep::Result;
        if (!batch_ || step != next_) throw std::invalid_argument("native work does not follow allocation/model/read completion");
        auto effects = kind == "result" ? result_effects(message.at("members")) : BatchEffects{};
        return ServingAction{
            { group_, sequence_ },
            std::make_shared<BatchWork>(*batch_, std::move(effects)),
            now_ns,
            flow_,
            0,
            kind == "model" ? std::optional<std::size_t>(0) : std::nullopt,
            step
        };
    }
    return std::nullopt;
}

nlohmann::json NativeGroup::resource_call(const nlohmann::json & message, std::size_t delivered) {
    const auto kind = message.at("kind").get<std::string>();
    if (kind == "free_slots") return resources_.free_slots();
    if (kind == "capacity")
        return {
            { "slots", resources_.free_slots().size() },
            { "pages", resources_.free_pages().size() }
        };
    if (kind == "slots" || kind == "allocate") {
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
        auto allocation = resources_.plan(phase, demands);
        resources_.reserve(allocation);
        auto bindings = nlohmann::json::array();
        for (const auto & member : allocation.members) {
            bindings.push_back({
                { "slot", member.slot },
                { "indices", resources_.indices(member.request, member.context_tokens, member.context_tokens + member.new_tokens) }
            });
        }
        if (kind == "allocate") batch_ = std::move(allocation);
        return bindings;
    }
    if (kind == "release_request") {
        const auto identity = message.at("request").get<std::size_t>();
        if (next_ != ServingStep::Result || !resources_.residence(identity).slot || !released_requests_.insert(identity).second)
            throw std::invalid_argument("native slot release is not an active result effect");
        return nullptr;
    }
    if (kind == "release_pages") {
        if (next_ != ServingStep::Result && next_ != ServingStep::Stage)
            throw std::invalid_argument("native page release is outside preparation or result processing");
        const auto pages = message.at("pages").get<std::vector<std::size_t>>();
        const auto position = next_ == ServingStep::Stage || batch_->phase == Phase::Prefill ? released_pages_.begin() : released_pages_.end();
        released_pages_.insert(position, pages.begin(), pages.end());
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

BatchEffects NativeGroup::result_effects(const nlohmann::json & members) const {
    if (members.size() != batch_->members.size()) throw std::invalid_argument("native result membership differs from submitted work");
    BatchEffects effects;
    for (std::size_t index = 0; index < members.size(); ++index) {
        const auto & input = members.at(index);
        const auto & member = batch_->members[index];
        const auto generated = input.at("generated_tokens").get<std::int64_t>();
        const bool finished = input.at("finished").get<bool>();
        const auto & request = requests_.at(member.request);
        const auto expected = member.context_tokens + member.new_tokens - request.prompt_tokens + 1;
        if (input.at("request") != member.request || generated <= 0 || generated > request.output_tokens || generated != expected
            || finished != (generated == request.output_tokens) || finished != released_requests_.contains(member.request))
            throw std::invalid_argument("native result does not match request progress or resource release");
        effects.members.push_back({ member.request,
                                    member.slot,
                                    generated,
                                    member.context_tokens + member.new_tokens,
                                    finished,
                                    input.at("emits_output").get<bool>(),
                                    finished ? resources_.residence(member.request).pages : std::vector<std::size_t>{} });
    }
    if (released_requests_.size() != static_cast<std::size_t>(std::ranges::count_if(effects.members, &RequestCompletion::finished)))
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
    if (action.step == ServingStep::Stage) next_ = ServingStep::ReadResult;
    else if (action.step == ServingStep::ReadResult) next_ = ServingStep::Result;
    else if (action.step == ServingStep::Release) {

        // Retraction frees residency, not request progress; native code decides when to retry Prefill.
        if (!std::ranges::equal(returned_pages, released_pages_)) throw std::invalid_argument("native retraction differs from release graph effects");
        resources_.release(action.work->batch.members.front().request);
        resources_.return_pages(returned_pages);
        released_pages_.clear();
    }
    else if (action.step == ServingStep::Result) {

        // Native effects describe releases; graph completion makes those resources reusable.
        if (!std::ranges::equal(returned_pages, released_pages_)) throw std::invalid_argument("native releases differ from result graph effects");
        for (const auto & member : action.work->effects.members) {
            resources_.advance(member.request, member.resident_tokens);
            if (member.finished) {
                resources_.release(member.request);
                ++completed_;
            }
        }
        resources_.return_pages(returned_pages);
        released_requests_.clear();
        released_pages_.clear();
        batch_.reset();
        next_ = ServingStep::Batch;
        ++flow_;
    }
}

bool NativeGroup::done() const { return completed_ == requests_.size() && !running_; }
std::size_t NativeGroup::free_pages() const { return resources_.free_pages().size(); }
std::size_t NativeGroup::sequence() const { return sequence_; }

} // namespace markov
