#include "markov/production/execution_source.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>

namespace markov {

class ExecutionSource::Builder {
public:
    explicit Builder(ExecutionSource & owner) : owner_(owner) {}

    ExecutionPrototype compile(const nlohmann::json & observation) {
        const auto & host = observation.at("host_scope");
        if (!host.is_null()) {
            const auto lane = resource("thread", host.at("thread").get<std::int64_t>());
            scopes_[lane].push_back({ host.at("order").get<std::size_t>(), host.at("completion_order").get<std::size_t>(), std::nullopt, std::nullopt });
        }

        std::set<std::size_t> observed_submissions;
        for (const auto & work : observation.at("device_work")) {
            if (!work.at("submission").is_null()) observed_submissions.insert(work.at("submission").at("source_event").get<std::size_t>());
        }

        std::set<std::size_t> event_submissions;

        // Runtime entries may also submit an observed device task: both use one source node.
        for (const auto & action : observation.at("runtime_actions")) {
            const auto & submission = action.at("submission");
            if (submission.is_null()) continue;

            const auto node = submit(submission, action.at("kind").get<std::string>(), action.at("native_api").get<std::string>());
            if (action.at("kind") == "record_event" || action.at("kind") == "wait_event")
                event_submissions.insert(submission.at("source_event").get<std::size_t>());
            const auto & target = action.at("target");
            if (!target.is_null()) nodes_[node]["target"] = resource(target.at("kind").get<std::string>(), target.at("identity").get<std::int64_t>());

            if (action.at("kind") == "copy_async" && !target.is_null() && !observed_submissions.contains(submission.at("source_event").get<std::size_t>())) {
                const auto transfer = add("transfer",
                                          submission.at("source_event").get<std::size_t>(),
                                          submission.at("order").get<std::size_t>(),
                                          "transfer",
                                          nodes_[node].at("target").get<std::size_t>(),
                                          "transfer");
                issues_[transfer] = issues_.at(node);
                link(node, transfer, "submission");
            }

            if (action.at("kind") == "wait_stream" || action.at("kind") == "wait_device") {
                const auto & frontiers = action.at("wait_frontiers");
                if (target.is_null() || frontiers.is_null()) throw std::invalid_argument("native wait has no completion interface");

                Wait wait{ entries_.at(node), node, {}, {} };
                for (const auto & queue : frontiers.at("queues")) wait.queues.push_back(resource("thread", queue.get<std::int64_t>()));
                for (const auto & stream : frontiers.at("streams")) wait.streams.push_back(resource("stream", stream.get<std::int64_t>()));
                waits_.push_back(std::move(wait));
            }
        }

        std::vector<const nlohmann::json *> transport;
        for (const auto & work : observation.at("device_work")) {
            const auto & submission = work.at("submission");
            if (!submission.is_null() && event_submissions.contains(submission.at("source_event").get<std::size_t>())) continue;
            if (work.value("communication", false)) {
                transport.push_back(&work);
                continue;
            }

            const auto node = add("device",
                                  work.at("source_event").get<std::size_t>(),
                                  submission.is_null() ? work.at("order").get<std::size_t>() : submission.at("order").get<std::size_t>(),
                                  work.at("kind").get<std::string>(),
                                  resource("stream", work.at("stream").get<std::int64_t>()),
                                  work.at("name").get<std::string>(),
                                  work.at("order").get<std::size_t>());
            if (!submission.is_null()) {
                const auto origin = submit(submission, "submit", submission.at("native_entry").get<std::string>());
                issues_[node] = issues_.at(origin);
                link(origin, node, "submission");
            }
        }

        bind_transport(transport, !observation.at("communication").is_null());
        bind_events(observation.at("runtime_actions"));

        auto sequences = nlohmann::json::array();
        for (auto & positions : positions_) {
            std::ranges::sort(positions);
            std::vector<std::size_t> sequence;
            std::optional<std::pair<std::size_t, std::size_t>> previous;
            for (const auto & [primary, secondary, node] : positions) {
                const auto order = std::pair{ primary, secondary };
                if (previous == order) throw std::invalid_argument("source execution actions have ambiguous order on one resource");
                previous = order;
                sequence.push_back(node);
            }
            sequences.push_back(std::move(sequence));
        }

        // Wait requirements refer to the work frontier visible at the native call boundary.
        auto requirements = std::move(event_requirements_);
        for (const auto & wait : waits_) {
            for (const auto queue : wait.queues) requirements.push_back(requirement(queue, wait.entry, "queue_drain"));
            for (const auto stream : wait.streams) requirements.push_back(requirement(stream, wait.node, "wait_completion"));
        }

        for (std::size_t lane = 0; lane < actions_.size(); ++lane) {
            if (!actions_[lane].empty() || !scopes_[lane].empty()) sequences[lane] = host_sequence(lane);
        }

        return {
            { { "nodes", std::move(nodes_) },
             { "links", std::move(links_) },
             { "sequences", std::move(sequences) },
             { "requirements", std::move(requirements) },
             { "signals", std::move(signals_) },
             { "communication", std::move(communication_) },
             { "host_call", std::move(host_call_) } },
            std::move(bindings_)
        };
    }

private:
    void bind_transport(std::vector<const nlohmann::json *> & work, bool declared) {
        if (!declared && work.empty()) return;
        if (!declared || work.empty()) throw std::invalid_argument("communication interface has no matching transport work");
        std::ranges::sort(work, [](const auto * left, const auto * right) { return left->at("order") < right->at("order"); });

        const auto lane = resource("stream", work.front()->at("stream").get<std::int64_t>());
        std::set<std::size_t> submissions;
        for (const auto * item : work) {
            if (resource("stream", item->at("stream").get<std::int64_t>()) != lane)
                throw std::invalid_argument("one communication interface spans multiple transport streams");
            const auto & submission = item->at("submission");
            submissions.insert(submit(submission, "submit", submission.at("native_entry").get<std::string>()));
        }

        std::vector<std::size_t> ports;
        for (std::size_t index = 0; index < 2; ++index) {
            const auto & boundary = *(index == 0 ? work.front() : work.back());
            const auto & submission = boundary.at("submission");
            const auto node = add(index == 0 ? "communication_entry" : "communication_exit",
                                  boundary.at("source_event").get<std::size_t>(),
                                  submission.at("order").get<std::size_t>(),
                                  "barrier",
                                  lane,
                                  index == 0 ? "collective_entry" : "collective_exit",
                                  index);
            ports.push_back(node);
            issues_[node] = issues_.at(identities_.at({ "thread", submission.at("source_event").get<std::size_t>() }));
        }

        for (const auto origin : submissions) link(origin, ports.front(), "submission");
        communication_ = {
            {  "entry", ports.front() },
            { "return",  ports.back() }
        };
    }

    void bind_events(const nlohmann::json & actions) {
        std::vector<const nlohmann::json *> events;
        for (const auto & action : actions) {
            if (action.at("kind") == "record_event" || action.at("kind") == "wait_event") events.push_back(&action);
        }
        std::ranges::sort(events,
                          [](const auto * left, const auto * right) { return left->at("submission").at("order") < right->at("submission").at("order"); });

        std::map<std::size_t, std::size_t> recorded;
        for (const auto * action : events) {
            const auto & arguments = action->at("native_call");
            const auto & target = action->at("target");
            if (arguments.is_null() || !arguments.contains("event") || target.is_null() || target.at("kind") != "stream")
                throw std::invalid_argument("stream event has no observed identity or stream");

            const auto stream = resource("stream", target.at("identity").get<std::int64_t>());
            const auto source = action->at("source_event").get<std::size_t>();
            const auto origin = identities_.at({ "thread", source });

            // Event APIs define stream barriers even when the profiler omits their device tasks.
            const auto order = action->at("submission").at("order").get<std::size_t>();
            const auto barrier = add("event", source, order, "barrier", stream, action->at("native_api").get<std::string>());
            issues_[barrier] = issues_.at(origin);
            link(origin, barrier, "submission");
            const auto event = resource("event", arguments.at("event").get<std::int64_t>());

            // Bind the event's device completion, never its CPU enqueue or API return.
            if (action->at("kind") == "record_event") recorded.insert_or_assign(event, barrier);
            else if (const auto signal = recorded.find(event); signal != recorded.end()) link(signal->second, barrier, "event_wait");
            else
                event_requirements_.push_back({
                    { "resource",        event },
                    {   "source",      nullptr },
                    {   "target",      barrier },
                    {     "kind", "event_wait" }
                });
        }

        for (const auto & [event, node] : recorded)
            signals_.push_back({
                { "resource", event },
                {   "source",  node }
            });
    }

    std::size_t resource(const std::string & kind, std::int64_t identity) {
        const auto source = owner_.resource(kind, identity);
        const auto [position, added] = resources_.try_emplace(source, resources_.size());
        if (added) {
            bindings_.push_back(source);
            positions_.emplace_back();
            actions_.emplace_back();
            scopes_.emplace_back();
        }

        return position->second;
    }

    std::size_t add(const std::string & domain, std::size_t source, std::size_t order, const std::string & kind, std::size_t lane, const std::string & name,
                    std::size_t secondary = 1) {
        const auto [position, added] = identities_.try_emplace(std::pair{ domain, source }, nodes_.size());
        if (added) {
            node(kind, lane, name);
            positions_[lane].emplace_back(order, secondary, position->second);
        }

        return position->second;
    }

    std::size_t node(const std::string & kind, std::size_t lane, const std::string & name) {
        const auto identity = nodes_.size();
        nodes_.push_back({
            {     "kind", kind },
            {     "name", name },
            { "resource", lane }
        });
        return identity;
    }

    void link(std::size_t source, std::size_t target, const std::string & kind) {
        links_.push_back({
            { "source", source },
            { "target", target },
            {   "kind",   kind }
        });
    }

    std::size_t submit(const nlohmann::json & submission, const std::string & kind, const std::string & name) {
        const auto source = submission.at("source_event").get<std::size_t>();
        if (const auto known = identities_.find({ "thread", source }); known != identities_.end()) return known->second;

        const auto begin = submission.at("order").get<std::size_t>();
        const bool waiting = kind == "wait_stream" || kind == "wait_device";
        const auto end = waiting || kind == "copy" ? submission.at("completion_order").get<std::size_t>() : begin;
        const auto lane = resource("thread", submission.at("thread").get<std::int64_t>());
        const auto identity = add("thread", source, end, kind, lane, name);
        const auto thread = submission.at("thread").get<std::int64_t>();
        issues_[identity] = { thread, begin, thread, begin };

        HostAction action{ begin, end, {} };
        if (waiting) {
            const auto entry = node("wait_entry", lane, name);
            entries_[identity] = entry;
            issues_[entry] = issues_.at(identity);
            action.nodes.push_back(entry);
        }

        // shortcut: place API self work before its return barrier; refine if measured internal overlap matters.
        if (waiting || kind == "copy") action.nodes.push_back(node("host", lane, waiting ? "synchronization" : "copy_call"));
        action.nodes.push_back(identity);
        actions_[lane].push_back(std::move(action));

        const auto & queue = submission.at("queue");
        if (!queue.is_null()) {
            const auto enqueue_source = queue.at("enqueue_event").get<std::size_t>();
            const bool new_item = !identities_.contains({ "thread", enqueue_source });
            const auto producer = resource("thread", queue.at("producer_thread").get<std::int64_t>());
            const auto enqueue = add("thread", enqueue_source, queue.at("enqueue_order").get<std::size_t>(), "enqueue", producer, "enqueue");
            issues_[identity].thread = queue.at("producer_thread").get<std::int64_t>();
            issues_[identity].order = queue.at("enqueue_order").get<std::size_t>();

            // Queue drain includes the native item's CPU tail after its last API submission.
            const auto returned = add("queue",
                                      queue.at("dequeue_event").get<std::size_t>(),
                                      queue.at("dequeue_completion_order").get<std::size_t>(),
                                      "queue_return",
                                      lane,
                                      "queue_return",
                                      0);
            issues_[returned] = issues_.at(identity);

            if (new_item) {
                const auto order = queue.at("enqueue_order").get<std::size_t>();
                actions_[producer].push_back({ order, order, { enqueue } });
                scopes_[lane].push_back(
                    { queue.at("dequeue_order").get<std::size_t>(), queue.at("dequeue_completion_order").get<std::size_t>(), enqueue, returned });
            }
        }

        return identity;
    }

    std::vector<std::size_t> host_sequence(std::size_t lane) {
        auto & actions = actions_[lane];
        auto & scopes = scopes_[lane];
        std::ranges::sort(actions, {}, &HostAction::begin);
        std::ranges::sort(scopes, {}, &HostScope::begin);

        std::vector<std::size_t> result;
        std::size_t next = 0;
        std::size_t previous_end = 0;
        for (const auto & scope : scopes) {
            if (scope.end < scope.begin || scope.begin < previous_end || (next < actions.size() && actions[next].begin < scope.begin))
                throw std::invalid_argument("native host scopes overlap or do not contain their actions");

            previous_end = scope.end;
            const auto first = nodes_.size();
            auto cursor = scope.begin;
            const auto name = scope.enqueue ? "worker" : "caller";

            while (next < actions.size() && actions[next].begin <= scope.end) {
                const auto & action = actions[next++];
                if (action.begin < cursor || action.end > scope.end) throw std::invalid_argument("native host actions overlap or exceed their scope");
                result.push_back(node("host", lane, name));
                result.insert(result.end(), action.nodes.begin(), action.nodes.end());
                cursor = action.end;
            }

            const auto returned = node("host", lane, name);
            result.push_back(returned);
            if (scope.enqueue) {

                // The worker owns only the queue item's service, never idle time between items.
                link(*scope.enqueue, first, "enqueue");
                result.push_back(*scope.returned);
            }
            else {
                host_call_ = {
                    {  "entry",    first },
                    { "return", returned }
                };
            }
        }

        if (next != actions.size()) throw std::invalid_argument("host action has no native scope");
        return result;
    }

    nlohmann::json requirement(std::size_t lane, std::size_t target, const std::string & kind) const {
        nlohmann::json source;
        const auto & wait = issues_.at(target);

        // Within a worker, preserve earlier submissions even when they share one queue item.
        for (const auto & [_, secondary, node] : positions_[lane]) {
            if (kind == "queue_drain" && nodes_[node].at("kind") != "queue_return") continue;
            const auto issue = issues_.find(node);
            if (issue == issues_.end()) continue;

            const auto & issued = issue->second;
            const bool before =
                issued.actor_thread == wait.actor_thread ? issued.actor_order < wait.actor_order : issued.thread == wait.thread && issued.order < wait.order;
            if (before) source = node;
        }

        return {
            { "resource",              lane },
            {   "source", std::move(source) },
            {   "target",            target },
            {     "kind",              kind }
        };
    }

    struct Issue {
        std::int64_t thread;
        std::size_t order;
        std::int64_t actor_thread;
        std::size_t actor_order;
    };

    struct Wait {
        std::size_t entry;
        std::size_t node;
        std::vector<std::size_t> queues;
        std::vector<std::size_t> streams;
    };

    struct HostAction {
        std::size_t begin;
        std::size_t end;
        std::vector<std::size_t> nodes;
    };

    struct HostScope {
        std::size_t begin;
        std::size_t end;
        std::optional<std::size_t> enqueue;
        std::optional<std::size_t> returned;
    };

    ExecutionSource & owner_;
    std::map<std::size_t, std::size_t> resources_;
    std::vector<std::size_t> bindings_;
    std::map<std::pair<std::string, std::size_t>, std::size_t> identities_;

    std::vector<std::vector<std::tuple<std::size_t, std::size_t, std::size_t>>> positions_;
    std::vector<std::vector<HostAction>> actions_;
    std::vector<std::vector<HostScope>> scopes_;
    std::map<std::size_t, std::size_t> entries_;
    std::map<std::size_t, Issue> issues_;
    std::vector<Wait> waits_;

    nlohmann::json nodes_ = nlohmann::json::array();
    nlohmann::json links_ = nlohmann::json::array();
    nlohmann::json host_call_;
    nlohmann::json signals_ = nlohmann::json::array();
    nlohmann::json event_requirements_ = nlohmann::json::array();
    nlohmann::json communication_;
};

std::size_t ExecutionSource::resource(const std::string & kind, std::int64_t identity) {
    const auto [position, added] = resources_.try_emplace(std::pair{ kind, identity }, resources_.size());
    if (added) kinds_.push_back(kind);
    return position->second;
}

ExecutionPrototype ExecutionSource::compile(const nlohmann::json & observation) { return Builder(*this).compile(observation); }
const std::vector<std::string> & ExecutionSource::resources() const { return kinds_; }

} // namespace markov
