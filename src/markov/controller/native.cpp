#include "markov/controller/native.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace markov {

NativeController::NativeController(std::vector<Request> requests, std::span<const NativeInstance> instances, ControlPeer & peer,
                                   const nlohmann::json & prefixes, Timeline & timeline, const nlohmann::json & ingress,
                                   std::int64_t given_work_ns, ExecutionInputs * execution_inputs, ResourceObserver * observer,
                                   ControlObserver * control_observer)
    : requests_(std::move(requests)),
      peer_(peer),
      timeline_(timeline),
      execution_inputs_(execution_inputs),
      observer_(observer),
      control_observer_(control_observer),
      cpu_(timeline, given_work_ns, control_observer, execution_inputs) {
    auto configuration = nlohmann::json::array();
    if (!ingress.is_null() && (!ingress.is_object() || ingress.size() != instances.size()))
        throw std::invalid_argument("ingress must describe every serving instance");
    for (const auto & instance : instances) {
        inputs_.emplace(instance.id, Input{ RequestDelivery(requests_, ingress.is_null() ? nullptr : &ingress.at(instance.id)) });
        auto bindings = nlohmann::json::array();
        for (const auto & participant : instance.participants) {
            groups_.emplace(participant.group, Participant{ participant.resources, participant.graphs, participant.dependencies });
            completed_.try_emplace(participant.group);
            instances_.emplace(participant.group, instance.id);
            cpu_.add_actor(participant.group, { instance.id, participant.control });
            bindings.push_back({
                {   "id", participant.group },
                { "rank",  participant.rank }
            });
        }
        const auto & settings = instance.settings;
        configuration.push_back({
            {       "id",                                                                                      instance.id                         },
            {   "groups",                                                                                                       std::move(bindings) },
            { "settings",
             { { "max_running_requests", settings.max_running_requests },
             { "max_total_tokens", settings.max_total_tokens },
             { "page_size", settings.page_size },
             { "max_prefill_tokens", settings.max_prefill_tokens },
             { "prefill_chunk_tokens", instance.prefill_chunk_tokens },
             { "mixed_batches", instance.mixed_batches },
             { "expert_routes", instance.expert_routes },
             { "microbatch_requests", instance.microbatch_requests ? nlohmann::json(*instance.microbatch_requests) : nlohmann::json(nullptr) } }   }
        });
    }
    if (execution_inputs_) {
        std::vector<std::string> names;
        for (const auto & [name, _] : groups_) names.push_back(name);
        execution_inputs_->bind(names);
    }
    auto content = nlohmann::json::array();
    for (const auto & request : requests_) {
        nlohmann::json value{
            {            "id",            request.id },
            { "prompt_tokens", request.prompt_tokens },
            { "output_tokens", request.output_tokens }
        };
        if (request.prefix_id) value["prefix_id"] = *request.prefix_id;
        content.push_back(std::move(value));
    }
    if (control_observer_) control_observer_->initialized(configuration, content, control_clock_epoch_ns);
    peer_.send({
        {      "kind",                                                      "configure" },
        { "observe", control_observer_ != nullptr },
        { "clock_epoch_ns", control_clock_epoch_ns },
        { "instances",                                         std::move(configuration) },
        {  "workload", { { "requests", std::move(content) }, { "prefixes", prefixes } } }
    });

    // No workload is delivered while native participants establish their empty control state.
    bool initialized = false;
    std::map<std::string, std::string> ingress_groups;
    while (idle_.size() != groups_.size()) {
        const auto call = receive_call();
        if (call.at("kind") == "waiting") {

            // Pipeline peers can wait for their next metadata round while each ingress is already parked at idle.
            if (initialized && ingress_groups.size() == inputs_.size() && std::ranges::all_of(ingress_groups, [&](const auto & entry) {
                    const auto idle = idle_.find(entry.second);
                    return idle != idle_.end() && idle->second.has_value();
                }))
                break;
            auto observed = nlohmann::json::array();
            for (const auto & [name, _] : idle_) observed.push_back(name);
            throw std::logic_error("native priming stopped: " + nlohmann::json{ { "observed_idle", observed } }.dump());
        }
        const auto & message = call.at("message");
        const auto kind = message.at("kind").get<std::string>();
        if (kind == "ready") {
            initialized = true;
            continue;
        }
        const auto & name = message.at("group").get_ref<const std::string &>();
        const auto identity = call.at("id").get<std::size_t>();
        if (cpu_call(identity, name, message)) continue;
        if (kind == "idle") {
            reach_idle(name, identity, message.at("waiting").get<bool>());
        }
        else if (kind == "input") {
            ingress_groups.insert_or_assign(instances_.at(name), name);
            reply(identity,
                  {
                      {     "kind",               "advance" },
                      {   "now_ns",                       0 },
                      { "arrivals", nlohmann::json::array() }
            });
        }
        else reply(identity, resource_call(name, message));
    }
    cpu_.prime();
}

bool NativeController::cpu_call(std::size_t call, const std::string & group, const nlohmann::json & message) {
    if (message.at("kind") != "cpu_post" && message.at("kind") != "cpu_wait") return false;
    if (message.at("execution").at("in_flow").get<bool>()) idle_.erase(group);
    cpu_.call(call, group, message);
    release_cpu();
    return true;
}

bool NativeController::release_cpu() {
    const auto replies = cpu_.take_replies();
    for (const auto & [call, result] : replies) reply(call, result);
    return !replies.empty();
}

nlohmann::json NativeController::receive_call() {
    peer_.send({
        { "kind", "poll" }
    });
    return peer_.receive();
}

void NativeController::reply(std::size_t call, const nlohmann::json & result) {
    peer_.send({
        {  "reply",   call },
        { "result", result }
    });
}

void NativeController::reach_idle(const std::string & group, std::size_t call, bool waiting) {

    // Native ingress sleeps on its own idle boundary; peers finish their current turns independently.
    idle_.insert_or_assign(group, waiting ? std::optional(call) : std::nullopt);
}

bool NativeController::release_idle(const std::string & instance) {
    bool resumed = false;
    for (auto group = idle_.begin(); group != idle_.end();) {
        if (instances_.at(group->first) != instance) {
            ++group;
            continue;
        }
        if (group->second) {
            reply(*group->second, nullptr);
            resumed = true;
        }
        group = idle_.erase(group);
    }
    return resumed;
}

std::optional<ServingAction> NativeController::plan(std::int64_t now_ns) {
    while (!done()) {
        resolve_notifications();
        for (auto & [instance, input] : inputs_) {
            const bool arrived = input.delivery.ready(now_ns);

            // Real progress wakes either park; consume it only when a sleeping native actor actually resumes.
            if (!shutdown_ && (arrived || input.woken) && release_idle(instance)) input.woken = false;
            if (!input.call) continue;

            if (input.return_ns && *input.return_ns < now_ns)
                throw std::invalid_argument("native input read reached after its observed return: " + input.identity.dump());

            // A measured read completes at its own boundary; it never prescribes which requests or batch to choose.
            // The adapter declares a safe wait; submitting another polling graph is not logical progress.
            const bool ready = input.return_ns ? *input.return_ns == now_ns : arrived || input.woken || !input.waiting;
            if (ready) {
                const auto arrivals = input.delivery.take(now_ns);
                const bool in_flow = input.in_flow || !arrivals.empty();

                // The first nonempty pull enters serving; idle polls after local drain remain observable outside it.
                if (control_observer_) control_observer_->input_read(input.identity, arrivals, in_flow, now_ns);
                reply(*input.call,
                      {
                          {     "kind",           "advance" },
                          {   "now_ns",              now_ns },
                          { "arrivals", arrivals }
                });
                input.call.reset();
                input.return_ns.reset();

                // Peers can publish midway through a PP round; keep that wake until the next declared wait consumes it.
                if (input.waiting) input.woken = false;
            }
        }
        const auto call = receive_call();
        if (call.at("kind") == "waiting") {
            if (shutdown_ || !timeline_.done() || !running_calls_.empty()
                || std::ranges::any_of(inputs_, [](const auto & entry) { return entry.second.return_ns.has_value(); })
                || !std::ranges::all_of(completed_, [&](const auto & entry) { return entry.second.size() == requests_.size(); }))
                return std::nullopt;

            if (idle_.size() != groups_.size()) {

                // Native peers can need another empty control turn before their final idle housekeeping.
                std::set<std::string> draining;
                for (const auto & [group, _] : groups_) {
                    if (!idle_.contains(group)) draining.insert(instances_.at(group));
                }
                bool resumed = false;
                for (auto & [instance, input] : inputs_) {
                    if (draining.contains(instance) && input.call) {
                        input.woken = true;
                        resumed = true;
                    }
                }
                for (auto & [group, idle_call] : idle_) {
                    if (idle_call && draining.contains(instances_.at(group))) {
                        reply(*idle_call, nullptr);
                        idle_call.reset();
                        resumed = true;
                    }
                }
                if (resumed) continue;
                return std::nullopt;
            }
            if (!cpu_.shutdown_ready()) return std::nullopt;

            // All native actors are suspended after serving cleanup, and no execution work can still release a caller.
            peer_.send({ { "kind", "shutdown" } });
            for (auto & [_, input] : inputs_) input.call.reset();
            idle_.clear();
            shutdown_ = true;
            continue;
        }
        const auto & message = call.at("message");
        const auto identity = call.at("id").get<std::size_t>();
        const auto kind = message.at("kind").get<std::string>();
        const auto name = message.at("group").get<std::string>();
        if (cpu_call(identity, name, message)) continue;
        auto & input = inputs_.at(instances_.at(name));
        if (kind == "input") {
            input.call = identity;
            input.waiting = message.at("waiting").get<bool>();
            input.in_flow = message.at("in_flow").get<bool>();
            const auto & context = message.at("context");
            if (!context.is_object()) throw std::invalid_argument("native input read requires its entering logical context");
            std::set<std::string> received;
            for (const auto request : input.delivery.delivered()) received.insert(requests_[request].id);

            // Equal empty queues before and after a workload are different read states; reuse the delivery owner's history.
            input.identity = { { "instance", instances_.at(name) }, { "context", context }, { "received", received } };
            const auto condition = execution_inputs_ ? execution_inputs_->input(name, input.identity, input.in_flow) : std::nullopt;
            if (condition) {
                input.return_ns = std::max(now_ns, condition->begin_ns);
                if (*input.return_ns > condition->end_ns) throw std::invalid_argument("native input entered after its supplied return window: " + input.identity.dump());
            }
            else input.return_ns.reset();
            continue;
        }
        if (kind == "wake") {
            for (const auto & group : message.at("groups")) inputs_.at(instances_.at(group.get<std::string>())).woken = true;
            continue;
        }
        auto & participant = groups_.at(name);
        if (kind == "completion_notice") {
            const auto owner = read_parameters({ { "request", message.at("request") } }).at("request");
            if (owner < 0 || !input.delivery.delivered().contains(static_cast<std::size_t>(owner)))
                throw std::invalid_argument("notification owner was not delivered to this instance");
            const auto request = static_cast<std::size_t>(owner);
            if (std::ranges::all_of(completed_, [&](const auto & entry) { return entry.second.contains(request); }))
                throw std::invalid_argument("notification belongs to an already completed request");

            const auto key = ExecutionInputs::notification_identity(message.at("notification"));
            const auto event = notification_event(key, request);
            auto & notice = notices_.at(event);
            if (notice.call) throw std::invalid_argument("notification declared twice: " + key.dump());
            std::set<std::size_t> prerequisites;
            for (const auto & dependency : message.at("after"))
                prerequisites.insert(notification_event(ExecutionInputs::notification_identity(dependency), request));
            notice.call = identity;
            notice.signals = message.at("signals").get<std::vector<std::string>>();
            notice.prerequisites.assign(prerequisites.begin(), prerequisites.end());
            pending_notices_.insert(event);
            continue;
        }
        if (kind == "action_values") {
            const auto boundary = message.at("boundary").get<std::size_t>();
            const auto & values = message.at("values");
            if (!control_observer_ || !timeline_.completed(boundary) || !values.is_object() || values.empty())
                throw std::invalid_argument("value observation requires a completed boundary and nonempty object");
            control_observer_->observed_values(name, boundary, values, now_ns);
            reply(identity, nullptr);
            continue;
        }
        if (kind == "query_result") {
            const auto boundary = message.at("boundary").get<std::size_t>();
            const auto & result = message.at("result");
            const auto & clocks = message.at("clocks");
            if (!control_observer_ || !timeline_.completed(boundary) || !clocks.is_array()
                || (!result.is_boolean() && !result.is_number_integer()))
                throw std::invalid_argument("query observation requires a completed read boundary and scalar result");
            control_observer_->queried(name, boundary, result, clocks, now_ns);
            reply(identity, nullptr);
            continue;
        }
        if (kind == "completed") {
            const auto signal = message.at("signal").get<std::string>();
            const auto producer = completion_event(signal);
            if (!producer) throw std::invalid_argument("native completion reference has no live producer: " + signal);
            const auto event = *producer;
            const bool completed = timeline_.completed(event);
            if (!message.at("observation").is_null()) {
                const auto boundary = message.at("observation").get<std::size_t>();
                if (!control_observer_ || !timeline_.completed(boundary))
                    throw std::invalid_argument("completion read observation requires a completed query boundary");
                control_observer_->completion_read(name, boundary, signal, event, completed, now_ns);
            }
            if (completed) participant.dependencies.observe_completion(event);
            else observed_[event].insert(instances_.at(name));
            reply(identity, completed);
            continue;
        }
        if (kind == "observed_completion") {
            const auto event = message.at("event").get<std::size_t>();
            if (!timeline_.completed(event)) throw std::invalid_argument("control observed a pending completion");
            participant.dependencies.observe_completion(event);
            reply(identity, nullptr);
            continue;
        }
        if (kind == "describe") {
            const auto layouts =
                participant.graphs.outputs(message.at("action").get<std::string>(), message.at("condition"), read_parameters(message.at("parameters")));
            auto result = nlohmann::json::object();
            for (const auto & [field, layout] : layouts)
                result[field] = {
                    { "dtype", layout.dtype },
                    { "shape", layout.shape }
                };

            // Describing a result neither submits graph work nor consumes pending resource changes.
            reply(identity, result);
            continue;
        }
        if (kind == "stopped") {
            if (completed_.at(name).size() != requests_.size()) throw std::invalid_argument("native participant stopped with unfinished requests");
            stopped_.insert(name);
            continue;
        }
        if (kind == "idle") {
            reach_idle(name, identity, message.at("waiting").get<bool>());
            continue;
        }
        if (kind == "action") {

            // A new serving action ends this participant's previously reported idle interval.
            idle_.erase(name);
            ServingAction action{
                { name, next_action_++ },
                message.at("action").get<std::string>(),
                message.at("condition"), read_parameters(message.at("parameters")),
                message.at("work"),      message.at("outputs")
            };
            if (action.name.empty() || !action.condition.is_object() || !action.work.is_object() || !action.outputs.is_array())
                throw std::invalid_argument("native action has an incomplete declaration");
            running_calls_.emplace(action.task, identity);
            return action;
        }
        reply(identity, resource_call(name, message));
    }
    return std::nullopt;
}

nlohmann::json NativeController::resource_call(const std::string & group, const nlohmann::json & message) {
    auto & participant = groups_.at(group);
    if (message.at("kind") == "clock") return timeline_.now_ns();

    if (message.at("kind") == "protocol") {
        if (control_observer_) control_observer_->native_protocol(group, message.at("record"), timeline_.now_ns());
        return nullptr;
    }
    auto result = participant.resources.call(message, inputs_.at(instances_.at(group)).delivery.delivered());

    // Another participant may write this pool before its owner's next action; do not defer invalidation to that action.
    const auto changes = participant.resources.take_changes();
    participant.dependencies.invalidate(changes);
    auto & input = inputs_.at(instances_.at(group));
    if (changes.capacity_increased) input.woken = true;
    if (observer_) observer_->applied(group, message, result, changes, timeline_.now_ns());
    return result;
}

std::optional<std::size_t> NativeController::completion_event(const std::string & signal) const {
    for (const auto & [_, owner] : groups_) {
        if (const auto event = owner.dependencies.signal(signal)) return *event;
    }
    return std::nullopt;
}

void NativeController::resolve_notifications() {
    for (auto pending = pending_notices_.begin(); pending != pending_notices_.end();) {
        auto & notice = notices_.at(*pending);
        std::set<std::size_t> prerequisites(notice.prerequisites.begin(), notice.prerequisites.end());
        bool submitted = true;
        for (const auto & signal : notice.signals) {
            const auto event = completion_event(signal);
            if (!event) {
                submitted = false;
                break;
            }
            prerequisites.insert(*event);
        }
        if (!submitted) {
            ++pending;
            continue;
        }

        // Bind when graph producers exist, before advancing time or waiting for the enclosing CPU return.
        notice.prerequisites.assign(prerequisites.begin(), prerequisites.end());
        timeline_.resolve(*pending, notice.prerequisites, Relation::Signal,
                          execution_inputs_ ? std::optional(execution_inputs_->notification(notice.identity)) : std::nullopt);
        notice.signals.clear();
        pending = pending_notices_.erase(pending);
    }
}

std::size_t NativeController::notification_event(const nlohmann::json & identity, std::size_t request) {
    if (!groups_.contains(identity.at("group").get<std::string>())) throw std::invalid_argument("notification refers to an unknown participant");
    const auto found = notification_ids_.find(identity);
    if (found != notification_ids_.end()) {
        if (notices_.at(found->second).request != request) throw std::invalid_argument("notification changes its request owner");
        return found->second;
    }

    // Receivers may declare their dependencies before the asynchronous producers have submitted work.
    const auto event = timeline_.defer();
    notification_ids_.emplace(identity, event);
    notices_.emplace(event, CompletionNotice{ identity, request, {}, {}, {} });
    if (control_observer_) control_observer_->notification_declared(identity, event);
    return event;
}

void NativeController::release_notifications(std::size_t request) {

    // A sender may finish before its receivers register; retirement waits for every participant's native cleanup.
    if (!std::ranges::all_of(completed_, [&](const auto & entry) { return entry.second.contains(request); })) return;
    std::erase_if(notices_, [&](const auto & entry) {
        const auto & [event, notice] = entry;
        if (notice.request != request) return false;
        if (!timeline_.completed(event)) throw std::invalid_argument("native request completed with a pending notification");
        notification_ids_.erase(notice.identity);
        return true;
    });
}

std::set<std::size_t> NativeController::retained_events() const {
    auto events = cpu_.retained_events();

    // Retain notifications while their request is live, including later registrations by another participant.
    for (const auto & [event, _] : notices_) events.insert(event);
    return events;
}

bool NativeController::notify(std::size_t event) {
    cpu_.notify(event);
    bool released = release_cpu();

    const auto notification = notices_.find(event);
    if (notification != notices_.end()) {
        const auto & notice = notification->second;
        if (control_observer_) control_observer_->notification_ready(notice.identity, event, notice.prerequisites, timeline_.now_ns());
        reply(*notice.call, { { "event", event } });
        released = true;
    }

    const auto observed = observed_.find(event);
    if (observed == observed_.end()) return released;

    // Completion wakes its actual observers, not every instance that happens to be polling.
    for (const auto & instance : observed->second) inputs_.at(instance).woken = true;
    observed_.erase(observed);
    return true;
}

void NativeController::finish(const ServingAction & action, std::size_t returned_event, std::int64_t completed_ns,
                              const std::map<std::string, StorageBinding> & values) {
    const auto call = running_calls_.at(action.task);
    auto & completed = completed_.at(action.task.group);
    auto & input = inputs_.at(instances_.at(action.task.group));
    for (const auto & output : action.outputs) {
        const auto request = output.at("request").get<std::size_t>();
        if (!input.delivery.delivered().contains(request))
            throw std::invalid_argument("native action published an undelivered request");

        // Publishing a final token can precede native resource cleanup and the result call's return.
        if (!output.at("emits_output").get<bool>() && output.at("finished").get<bool>()) {
            if (!completed.insert(request).second) throw std::invalid_argument("native action completed a request twice");
            input.woken = true;
            release_notifications(request);
        }
    }

    // Native results belong to this actual return, not to the source template's observed effects.
    if (control_observer_)
        control_observer_->observed_values(action.task.group, returned_event, {{ "outputs", action.outputs }}, completed_ns);

    auto bindings = nlohmann::json::object();
    for (const auto & [name, value] : values)
        bindings[name] = {
            { "identity", value.identity },
            {  "version",  value.version }
        };

    // Identity is available at CPU return; actual data readiness remains in the execution graph.
    nlohmann::json returned{ { "completed_ns", completed_ns }, { "values", std::move(bindings) } };
    if (control_observer_) returned["observation"] = returned_event;
    reply(call, returned);
    running_calls_.erase(action.task);
}

std::optional<std::int64_t> NativeController::next_wakeup_ns(std::int64_t now_ns) const {
    std::optional<std::int64_t> next;
    for (const auto & [_, input] : inputs_) {

        // Arrivals and measured read returns wake control independently; elapsed boundaries cannot stall the clock.
        for (const auto boundary : { input.delivery.next_ns(), input.return_ns }) {
            if (boundary && *boundary > now_ns && (!next || *boundary < *next)) next = boundary;
        }
    }
    return next;
}

bool NativeController::done() const { return stopped_.size() == groups_.size() && running_calls_.empty(); }
std::size_t NativeController::free_pages() const {
    std::size_t pages = 0;
    for (const auto & [name, group] : groups_) pages += group.resources.free_pages();
    return pages;
}
std::size_t NativeController::request_count() const { return requests_.size(); }
const Request & NativeController::request(std::size_t identity) const { return requests_.at(identity); }
nlohmann::json NativeController::cpu_summary() const { return cpu_.summary(); }

} // namespace markov
