#include "markov/controller/native_transfer.hpp"

#include <array>
#include <memory>
#include <stdexcept>
#include <utility>

namespace markov {

NativeTransfer::NativeTransfer(std::string source, std::string destination, KvPool & source_pool, KvPool & destination_pool)
    : source_(std::move(source)),
      destination_(std::move(destination)),
      source_pool_(source_pool),
      destination_pool_(destination_pool) {}

nlohmann::json NativeTransfer::call(const nlohmann::json & message) {
    const auto request = message.at("request").get<std::size_t>();
    auto & handoff = handoffs_[request];
    const auto kind = message.at("kind").get<std::string>();
    if (kind == "transfer_sender") handoff.sender = true;
    else if (kind == "transfer_receiver") handoff.receiver = true;
    else if (kind == "transfer_submit") {
        if (!handoff.payload.is_null()) throw std::invalid_argument("native transfer submitted twice");
        handoff.payload = message.at("payload");
    }
    else if (kind != "transfer_poll") throw std::invalid_argument("unknown native transfer call: " + kind);

    const bool ready = message.at("group") == source_ ? handoff.reserved : handoff.sender && handoff.receiver;
    const auto state = handoff.completed ? "completed" : ready ? "ready" : "bootstrapping";
    return {
        {   "state",                                                  state },
        { "payload", handoff.completed ? handoff.payload : nlohmann::json{} }
    };
}

std::optional<ServingAction> NativeTransfer::plan(std::int64_t now_ns) {
    for (auto & [request, handoff] : handoffs_) {
        if (!handoff.prepared || !handoff.reserved || handoff.payload.is_null() || handoff.submitted) continue;
        const std::array demands{
            KvDemand{ request, 0 }
        };
        auto batch = destination_pool_.plan(Phase::Prefill, demands);
        BatchWork work{ std::move(batch), {}, {} };
        const auto & source = source_pool_.residence(request).pages;
        const auto & destination = destination_pool_.residence(request).pages;
        for (std::size_t page = 0; page < source.size(); ++page) work.transferred_pages.push_back({ source[page], destination.at(page) });
        handoff.submitted = true;
        const auto sequence = sequence_++;
        return ServingAction{
            { "kv", sequence },
            std::make_shared<const BatchWork>(std::move(work)), now_ns, sequence, 0, std::nullopt, ServingStep::Transfer
        };
    }
    return std::nullopt;
}

void NativeTransfer::finish(const ServingAction & action) {
    if (action.step != ServingStep::Reserve && action.step != ServingStep::Transfer && !(action.step == ServingStep::Result && action.task.group == source_)
        && !(action.step == ServingStep::Release && action.task.group == source_) && !(action.step == ServingStep::Receive && !action.stage))
        return;
    for (const auto & member : action.work->batch.members) {
        auto & handoff = handoffs_[member.request];
        if (action.step == ServingStep::Reserve && action.task.group == destination_) handoff.reserved = true;
        if (action.step == ServingStep::Result && action.task.group == source_) handoff.prepared = true;
        if (action.step == ServingStep::Transfer) handoff.completed = true;
        if (action.step == ServingStep::Receive && !action.stage) handoff.received = true;
        if (action.step == ServingStep::Release && action.task.group == source_) handoff.released = true;
        if (handoff.received && handoff.released) handoffs_.erase(member.request);
    }
}

bool NativeTransfer::settled() const { return handoffs_.empty(); }

} // namespace markov
