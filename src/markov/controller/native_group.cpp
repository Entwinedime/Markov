#include "markov/controller/native_group.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>

namespace markov {
namespace {

nlohmann::json prefix_path(std::span<const PrefixStep> path) {
    auto result = nlohmann::json::array();
    for (const auto & step : path) {
        result.push_back({
            {           "node",                                                                        step.node },
            { "matched_tokens",                                                              step.matched_tokens },
            {   "split_prefix", step.split_prefix ? nlohmann::json(*step.split_prefix) : nlohmann::json(nullptr) }
        });
    }
    return result;
}

} // namespace

NativeGroup::NativeGroup(const std::vector<Request> & requests, KvPool & resources, std::string group, std::size_t stage, NativeRole role)
    : requests_(requests),
      resources_(resources),
      group_(std::move(group)),
      stage_(stage),
      role_(role) {}

void NativeGroup::begin_iteration(std::size_t flow, std::size_t lane) {
    if (const auto previous = lanes_.find(lane); previous != lanes_.end()) {
        if (preparations_.contains(previous->second)) throw std::invalid_argument("native lane reused before its batch was prepared");
        if (const auto frame = frames_.find(previous->second); frame != frames_.end() && frame->second.next != ServingStep::Batch)
            throw std::invalid_argument("native lane reused before its result completed");
    }

    // Output forwarding retains the old snapshot until its native transport user releases it.
    lanes_.insert_or_assign(lane, flow);
}

std::optional<ServingAction> NativeGroup::plan(const nlohmann::json & message, std::int64_t now_ns, std::size_t delivered) {
    const auto kind = message.at("kind").get<std::string>();
    if (kind == "index_work") {
        auto work = std::make_shared<IndexWork>(IndexWork::read(message));
        for (auto & operation : work->operations) {
            if (auto * read = std::get_if<IndexRead>(&operation)) {
                const auto & residence = resources_.residence(read->request);
                read->slot = residence.slot.value();
                read->pages = residence.pages;
            }
            if (auto * write = std::get_if<IndexWrite>(&operation)) write->slot = resources_.residence(write->request).slot.value();
        }
        return ServingAction{
            { group_, sequence_ },
            std::move(work), now_ns, message.at("flow").get<std::size_t>(), message.at("lane").get<std::size_t>(), stage_,
            ServingStep::Indices
        };
    }
    if (kind == "declare_slots" || kind == "capacity" || kind == "free_slots" || kind == "slots" || kind == "allocate" || kind == "reserve_pages"
        || kind == "prebuilt" || kind == "release_pages" || kind == "release_work" || kind == "prefix")
        return std::nullopt;
    const auto flow = message.at("flow").get<std::size_t>();
    const auto lane = message.at("lane").get<std::size_t>();
    if (kind == "transfer_submit") {
        if (role_ != NativeRole::Prefill) throw std::invalid_argument("only a source instance can submit KV");
        auto batch = resident_batch(nlohmann::json::array({ message.at("request") }), delivered);
        batch.phase = Phase::Prefill;
        auto & member = batch.members.front();
        member.phase = Phase::Prefill;
        const auto & resident = resources_.residence(member.request);
        const auto pages = message.at("pages").get<std::vector<std::size_t>>();
        const auto found = std::ranges::search(resident.pages, pages);
        if (pages.empty() || found.empty()) throw std::invalid_argument("native send must name a resident KV page range");
        member.context_tokens = (found.begin() - resident.pages.begin()) * resources_.page_size();
        member.new_tokens = std::min(resident.allocated_tokens, member.context_tokens + static_cast<std::int64_t>(pages.size()) * resources_.page_size())
                            - member.context_tokens;
        auto work = std::make_shared<BatchWork>(std::move(batch), BatchEffects{});
        work->final_transfer = message.at("final").get<bool>();
        const auto & sent = work->batch.members.front();
        if (work->final_transfer && sent.context_tokens + sent.new_tokens != requests_.at(sent.request).prompt_tokens)
            throw std::invalid_argument("final native KV send must cover the complete prompt");
        const auto slot = message.at("metadata_slot").get<std::int64_t>();
        if (slot < 0) throw std::invalid_argument("native send requires an allocated metadata slot");
        work->metadata_slots.emplace(work->batch.members.front().request, static_cast<std::size_t>(slot));
        return ServingAction{
            { group_, sequence_ },
            std::move(work), now_ns, flow, lane, stage_, ServingStep::Send
        };
    }
    if (kind == "reserve" || kind == "received") {
        const auto request = message.at("request").get<std::size_t>();
        std::shared_ptr<const BatchWork> work;
        if (kind == "reserve") work = reservations_.at(request);
        else {
            const auto slot = message.at("metadata_slot").get<std::int64_t>();
            if (slot < 0) throw std::invalid_argument("native receive requires an allocated metadata slot");
            auto batch = resident_batch(nlohmann::json::array({ request }), delivered);
            batch.phase = Phase::Prefill;
            batch.members.front().phase = Phase::Prefill;
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
    if (kind == "select" || kind == "retain" || (kind == "release_request" && (preparing || role_ == NativeRole::Prefill))) {
        if ((kind == "select" || kind == "retain") && !preparing) throw std::invalid_argument("native scheduling action is outside batch preparation");
        auto batch = resident_batch(kind == "select" ? message.at("requests") : nlohmann::json::array({ message.at("request") }), delivered);
        if (kind == "retain") {
            batch.phase = Phase::Prefill;
            batch.members.front().phase = Phase::Prefill;
        }
        BatchEffects effects;
        if (kind == "release_request") {
            const auto & member = batch.members.front();
            const std::set<std::size_t> released(released_pages_.begin(), released_pages_.end());
            std::vector<std::size_t> returned;
            for (const auto page : resources_.residence(member.request).pages) {
                if (released.contains(page)) returned.push_back(page);
            }
            effects.members.push_back({ member.request,
                                        member.slot,
                                        member.context_tokens - requests_.at(member.request).prompt_tokens + 1,
                                        member.context_tokens,
                                        true,
                                        false,
                                        std::move(returned),
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
            kind == "select"   ? ServingStep::Select
            : kind == "retain" ? ServingStep::Retain
                               : ServingStep::Release
        };
    }
    if (kind == "release_request") return std::nullopt;
    if (kind == "prepare") {
        auto & preparation = preparations_.at(flow);
        auto batch = preparation.allocations.front();
        for (std::size_t index = 1; index < preparation.allocations.size(); ++index) {
            const auto & part = preparation.allocations[index];
            if (part.phase != batch.phase) batch.phase = Phase::Mixed;
            batch.members.insert(batch.members.end(), part.members.begin(), part.members.end());
            batch.allocated_pages += part.allocated_pages;
        }
        if (lane != preparation.lane || message.at("phase") != phase_name(batch.phase))
            throw std::invalid_argument("native prepared batch differs from its allocation phase or lane");

        // Allocation reserves resources; only the final native batch publishes immutable model work.
        auto work = std::make_shared<BatchWork>(std::move(batch), BatchEffects{});
        if (preparation.allocations.size() > 1) work->preparations = std::move(preparation.allocations);
        Frame prepared{ std::move(work), lane, ServingStep::Prepare, {}, {}, {} };
        observe_progress(prepared, message.at("members"));
        frames_.emplace(flow, std::move(prepared));
        preparations_.erase(flow);
    }
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
        if (message.contains("index_tail")) result->index_tail = IndexWork::read(message.at("index_tail"));
        work = std::move(result);
    }
    return ServingAction{
        { group_, sequence_ },
        std::move(work), now_ns, flow, lane, stage_, step, message_step ? message.at("channel").get<std::string>() : std::string{}
    };
}

nlohmann::json NativeGroup::resource_call(const nlohmann::json & message, std::size_t delivered) {
    const auto kind = message.at("kind").get<std::string>();
    if (kind == "prefix") {
        auto & index = resources_.prefixes();
        const auto operation = message.at("operation").get<std::string>();
        if (operation == "match") {
            const auto result = index.match(message.at("tokens").get<std::vector<std::int64_t>>());
            return {
                { "node",              result.node },
                { "path", prefix_path(result.path) }
            };
        }
        if (operation == "insert") {
            const auto tokens = message.at("tokens").get<std::vector<std::int64_t>>();
            const auto pages = message.at("pages").get<std::vector<std::size_t>>();
            for (const auto page : pages) {
                if (page >= resources_.capacity().pages) throw std::invalid_argument("cached page is outside its resource pool");
            }
            const auto result = index.insert(tokens, pages);
            return {
                {            "node",              result.node },
                { "existing_tokens",   result.existing_tokens },
                {            "path", prefix_path(result.path) },
                {        "inserted",          result.inserted }
            };
        }
        if (operation == "lock") return index.lock(message.at("node").get<std::size_t>(), message.at("acquire").get<bool>());
        if (operation == "evict") return index.evict(message.at("tokens").get<std::int64_t>());
        throw std::invalid_argument("unknown prefix resource operation: " + operation);
    }
    if (kind == "declare_slots") {
        resources_.declare_slots(message.at("slots").get<std::size_t>());
        return nullptr;
    }
    if (kind == "free_slots") return resources_.free_slots();
    if (kind == "capacity")
        return {
            {            "slots",           resources_.free_slots().size() },
            {            "pages",           resources_.free_pages().size() },
            { "evictable_tokens", resources_.prefixes().evictable_tokens() },
            { "protected_tokens", resources_.prefixes().protected_tokens() }
        };
    if (kind == "slots" || kind == "allocate" || kind == "reserve_pages") {
        if (kind == "allocate") {
            const auto flow = message.at("flow").get<std::size_t>();
            if (frames_.contains(flow)) throw std::invalid_argument("native allocation replaced live model work");
            if (preparations_.contains(flow) && preparations_.at(flow).lane != message.at("lane"))
                throw std::invalid_argument("native allocations in one flow changed lane");
        }
        std::vector<KvDemand> demands;
        std::set<std::size_t> members;
        for (const auto & member : message.at("members")) {
            const auto identity = member.at("request").get<std::size_t>();
            if (identity >= delivered || !members.insert(identity).second)
                throw std::invalid_argument("native allocation has an undelivered or repeated request");
            const auto tokens = kind == "slots" ? 0 : member.at("tokens").get<std::int64_t>();
            if (tokens < 0) throw std::invalid_argument("native allocation has negative token demand");
            if (kind == "slots" && member.contains("prefix")) {
                const auto & prefix = member.at("prefix");
                resources_.bind_prefix(identity, prefix.at("node").get<std::size_t>(), prefix.at("tokens").get<std::int64_t>());
            }
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
            auto & preparation = preparations_.try_emplace(flow, Preparation{ {}, lane }).first->second;
            preparation.allocations.push_back(std::move(allocation));
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
            member.phase = Phase::Prefill;
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
        const auto position = result == nullptr || result->work->batch.phase != Phase::Decode ? released.begin() : released.end();
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

void NativeGroup::observe_progress(Frame & frame, const nlohmann::json & members) const {
    const auto & batch = frame.work->batch;
    if (members.size() != batch.members.size()) throw std::invalid_argument("native preparation membership differs from allocation");
    std::vector<std::int64_t> progress;
    std::set<std::size_t> identities;
    for (std::size_t index = 0; index < members.size(); ++index) {
        const auto & input = members.at(index);
        const auto & member = batch.members[index];
        const auto & request = requests_.at(member.request);
        const auto generated = input.at("generated_tokens").get<std::int64_t>();
        const auto end = member.context_tokens + member.new_tokens;
        if (input.at("request") != member.request || !identities.insert(member.request).second || input.at("phase") != phase_name(member.phase)
            || input.at("context_tokens") != member.context_tokens || input.at("new_tokens") != member.new_tokens || generated < 0
            || generated >= request.output_tokens || member.new_tokens <= 0 || end > request.prompt_tokens + generated
            || (member.phase == Phase::Decode && (member.new_tokens != 1 || end != request.prompt_tokens + generated)))
            throw std::invalid_argument("native preparation does not match its request input range");
        progress.push_back(generated);
    }

    // This snapshot records native progress only for this work; it is not a second request ledger.
    frame.generated_before = std::move(progress);
}

BatchEffects NativeGroup::result_effects(const Frame & frame, const nlohmann::json & members) const {
    const auto & batch = frame.work->batch;
    if (members.size() != batch.members.size()) throw std::invalid_argument("native result membership differs from submitted work");
    BatchEffects effects;
    const std::set<std::size_t> released(frame.released_pages.begin(), frame.released_pages.end());
    for (std::size_t index = 0; index < members.size(); ++index) {
        const auto & input = members.at(index);
        const auto & member = batch.members[index];
        const auto generated = input.at("generated_tokens").get<std::int64_t>();
        const bool finished = input.at("finished").get<bool>();
        const auto & request = requests_.at(member.request);
        const auto resident = member.context_tokens + member.new_tokens;
        const bool prebuilt = frame.next == ServingStep::Accept;
        const auto before = prebuilt ? resident - request.prompt_tokens : frame.generated_before.at(index);
        const bool continues_prefill = !prebuilt && resident < request.prompt_tokens + before;
        const auto expected = before + (continues_prefill ? 0 : 1);
        const bool emits_output = input.at("emits_output").get<bool>();
        if (input.at("request") != member.request || generated < 0 || (!continues_prefill && generated == 0) || generated > request.output_tokens
            || generated != expected || (continues_prefill && (finished || emits_output))
            || finished != (role_ != NativeRole::Prefill && generated == request.output_tokens) || finished != frame.released_requests.contains(member.request))
            throw std::invalid_argument("native result does not match request progress or resource release");
        std::vector<std::size_t> returned;
        if (!released.empty()) {
            for (const auto page : resources_.residence(member.request).pages) {
                if (released.contains(page)) returned.push_back(page);
            }
        }

        // A finished request releases its slot; only allocator-reported pages are returned to the pool.
        const bool reads_map = !continues_prefill && (finished || (!prebuilt && member.phase == Phase::Prefill));
        effects.members.push_back({ member.request,
                                    member.slot,
                                    generated,
                                    resident,
                                    finished,
                                    emits_output,
                                    std::move(returned),
                                    continues_prefill,
                                    reads_map ? resources_.residence(member.request).pages : std::vector<std::size_t>{} });
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
    if (action.step == ServingStep::Indices) {
        std::vector<std::size_t> expected;
        for (const auto & operation : action.index_work().operations) {
            if (const auto * release = std::get_if<IndexRelease>(&operation); release && release->mode != IndexReleaseMode::Deferred)
                expected.insert(expected.begin(), release->pages.begin(), release->pages.end());
        }
        if (!std::ranges::equal(expected, returned_pages)) throw std::invalid_argument("index work completion differs from native page releases");
        if (const auto & binding = action.index_work().binding) resources_.bind_prefix(binding->request, binding->node, binding->tokens);
        resources_.return_pages(returned_pages);
        return;
    }
    if (action.step == ServingStep::Select || action.step == ServingStep::Retain || (action.step == ServingStep::Send && action.channel.empty())) return;
    if (action.step == ServingStep::Receive) return;
    if (action.step == ServingStep::Reserve) {
        reservations_.erase(action.batch_work().batch.members.front().request);
        return;
    }
    if (action.step == ServingStep::Release) {
        if (!std::ranges::equal(returned_pages, released_pages_)) throw std::invalid_argument("native retraction differs from release graph effects");
        resources_.release(action.batch_work().batch.members.front().request);
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
        for (const auto & member : action.batch_work().effects.members) {
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
bool NativeGroup::settled() const { return done() && preparations_.empty() && frames_.empty() && reservations_.empty(); }
std::size_t NativeGroup::free_pages() const { return resources_.free_pages().size(); }

} // namespace markov
