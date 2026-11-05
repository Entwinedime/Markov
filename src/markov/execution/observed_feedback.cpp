#include "markov/execution/observed_feedback.hpp"

#include <limits>
#include <stdexcept>
#include <variant>

namespace markov {
namespace {

std::int64_t nonnegative(const nlohmann::json & value) {
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("observed boundary fields must be signed 64-bit integers");
    const auto result = value.get<std::int64_t>();
    if (result < 0) throw std::invalid_argument("observed boundary fields must be nonnegative");
    return result;
}

} // namespace

ObservedFeedback::ObservedFeedback(const nlohmann::json & observation, std::span<const Request> requests)
    : source_(observation.at("source").get<std::string>()) {
    if (source_.empty() || !observation.at("actions").is_array()) throw std::invalid_argument("observed feedback needs a source and action array");
    for (const auto & request : requests) requests_.push_back(request.id);

    for (const auto & row : observation.at("actions")) {
        const auto & work = row.at("work");
        const auto & publication = row.at("published");
        if (!work.is_object() || work.at("group").get<std::string>().empty())
            throw std::invalid_argument("observed feedback work must identify its control group");
        if (!row.at("observations").is_array()) throw std::invalid_argument("observed work references must be an array");
        const auto record = records_.size();
        records_.push_back({ work, !publication.is_null(), row.at("observations") });
        remaining_[work].push_back(record);
        const auto completion = add_event(row.at("completed"), record, ExecutionEvent::Kind::Completed);
        if (!publication.is_null() && add_event(publication, record, ExecutionEvent::Kind::Published) >= completion)
            throw std::invalid_argument("observed publication must precede its task completion");
    }
}

ObservedFeedback::EventOrder ObservedFeedback::add_event(const nlohmann::json & value, std::size_t record, ExecutionEvent::Kind kind) {
    const EventOrder key{ nonnegative(value.at("time_ns")), static_cast<std::size_t>(nonnegative(value.at("order"))) };
    if (!events_.emplace(key, Event{ record, kind }).second) throw std::invalid_argument("observed feedback repeats an event order at the same time");
    return key;
}

nlohmann::json ObservedFeedback::identity(const ServingAction & action) const {
    nlohmann::json result{
        { "group",                                                      action.task.group },
        {  "step",                                                     action.step_name() },
        {  "lane",                                                            action.lane },
        { "stage", action.stage ? nlohmann::json(*action.stage) : nlohmann::json(nullptr) }
    };
    if (action.step == ServingStep::Schedule) {
        result["operation"] = action.schedule_operation() == ScheduleOperation::Coordinate ? "coordinate" : "complete";
    }
    else if (action.step == ServingStep::Select) {
        result["view"] = action.selection().view == BatchView::Previous ? "previous" : "running";
        result["requests"] = nlohmann::json::array();
        for (const auto request : action.selection().requests) result["requests"].push_back(requests_.at(request));
    }
    else if (action.step == ServingStep::Poll) {
        result["requests"] = nlohmann::json::array();
        for (const auto request : action.transfer_query().requests) result["requests"].push_back(requests_.at(request));
    }
    else if (const auto * endpoint = std::get_if<TransferEndpoint>(&action.work)) {
        result["endpoint"] = {
            {   "group",                 endpoint->group },
            { "request", requests_.at(endpoint->request) }
        };
    }
    else if (action.step == ServingStep::Indices) {
        const auto & indices = action.index_work();
        result["services"] = nlohmann::json::array();
        std::size_t depth = 0;

        // A boundary identifies the outer cache service; its tensor implementation is checked separately.
        for (const auto & operation : indices.operations) {
            const auto * call = std::get_if<IndexCall>(&operation);
            if (!call) continue;
            if (call->enter) {
                if (depth == 0) result["services"].push_back(index_service_name(call->service));
                ++depth;
            }
            else --depth;
        }
        if (indices.binding)
            result["binding"] = {
                { "request", requests_.at(indices.binding->request) },
                {  "tokens",                indices.binding->tokens }
            };
        if (indices.prefix) result["prefix_request"] = requests_.at(indices.prefix->request);
        if (indices.release_request) result["release_request"] = requests_.at(*indices.release_request);
    }
    else {
        result["members"] = nlohmann::json::array();
        for (const auto & member : action.batch_work().batch.members)
            result["members"].push_back({
                {             "id", requests_.at(member.request) },
                {          "phase",     phase_name(member.phase) },
                { "context_tokens",        member.context_tokens },
                {     "new_tokens",            member.new_tokens }
            });
    }
    if (!action.channel.empty()) result["channel"] = action.channel;
    if (action.transfer)
        result["transfer"] = {
            {      "source",      action.transfer->source },
            { "destination", action.transfer->destination }
        };
    if (action.step == ServingStep::Transfer || (action.step == ServingStep::Send && action.channel.empty()))
        result["final_transfer"] = action.batch_work().final_transfer;
    return result;
}

void ObservedFeedback::submit(const RegionGraph &, const ServingAction & action, const BatchProgram & program) {
    const auto work = identity(action);
    const auto found = remaining_.find(work);
    if (found == remaining_.end())
        throw std::invalid_argument("observed boundary work differs at " + action.task.group + "/" + std::to_string(action.task.sequence)
                                    + ": no remaining observation for " + work.dump());

    // Source-local task numbers are not shared; match semantic work and its repeated occurrences.
    auto & record = records_.at(found->second.front());
    if (record.publishes != program.publication.has_value())
        throw std::invalid_argument("observed boundary and generated publication ports disagree for " + work.dump()
                                    + ": observed=" + std::to_string(record.publishes) + ", generated=" + std::to_string(program.publication.has_value()));
    record.task = action.task;
    found->second.pop_front();
    if (found->second.empty()) remaining_.erase(found);
}

std::optional<ExecutionEvent> ObservedFeedback::advance_until(std::optional<std::int64_t> deadline_ns) {
    if (deadline_ns && *deadline_ns < now_ns_) throw std::invalid_argument("observed feedback time must not move backwards");
    if (events_.empty() || (deadline_ns && *deadline_ns < events_.begin()->first.first)) {
        if (deadline_ns) now_ns_ = *deadline_ns;
        return std::nullopt;
    }
    const auto & [order, next] = *events_.begin();
    const auto & record = records_.at(next.record);
    if (order.first < now_ns_)
        throw std::invalid_argument("observed feedback precedes the current time: " + record.work.dump() + ", event_ns=" + std::to_string(order.first)
                                    + ", now_ns=" + std::to_string(now_ns_));
    if (!record.task)
        throw std::invalid_argument("observed feedback work has not been submitted: " + record.work.dump() + ", event_ns=" + std::to_string(order.first)
                                    + ", now_ns=" + std::to_string(now_ns_));
    const ExecutionEvent event{ order.first, *record.task, next.kind };
    now_ns_ = order.first;
    events_.erase(events_.begin());
    return event;
}

void ObservedFeedback::retain(std::span<const std::size_t>) {

    // This provider owns boundary records, not a fabricated internal execution timeline.
}

std::int64_t ObservedFeedback::now_ns() const { return now_ns_; }

nlohmann::json ObservedFeedback::finish(bool details) const {
    if (!remaining_.empty() || !events_.empty()) throw std::invalid_argument("serving finished before consuming its observed boundaries");
    nlohmann::json result{
        {      "completion_source",      "observed_boundaries" },
        { "completion_observation",                    source_ },
        {        "duration_source", "internal_nodes_not_timed" }
    };

    // Source references describe the completed match; execution never consumes them.
    if (details) {
        result["observed_work"] = nlohmann::json::array();
        for (const auto & record : records_)
            result["observed_work"].push_back({
                {         "task", { { "group", record.task->group }, { "sequence", record.task->sequence } } },
                { "observations",                                                        record.observations }
            });
    }
    return result;
}

} // namespace markov
