#include "markov/execution/timeline.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace markov {
namespace {

class SubmissionRoots {
public:
    explicit SubmissionRoots(const std::map<std::size_t, std::vector<std::size_t>> & incoming) : incoming_(incoming) {}

    const std::set<std::size_t> & at(std::size_t event) {
        const auto [entry, inserted] = roots_.try_emplace(event);
        if (!inserted) return entry->second;
        const auto parents = incoming_.find(event);
        if (parents == incoming_.end()) entry->second.insert(event);
        else {
            for (const auto parent : parents->second) {
                const auto & roots = at(parent);
                entry->second.insert(roots.begin(), roots.end());
            }
        }
        return entry->second;
    }

private:
    const std::map<std::size_t, std::vector<std::size_t>> & incoming_;
    std::map<std::size_t, std::set<std::size_t>> roots_;
};

std::int64_t after(std::int64_t time_ns, std::int64_t delay_ns) {
    if (delay_ns > std::numeric_limits<std::int64_t>::max() - time_ns) throw std::overflow_error("execution time exceeds int64 nanoseconds");
    return time_ns + delay_ns;
}

} // namespace

Timeline::Timeline(std::vector<Resource> resources, ExecutionObserver * observer)
    : resources_(std::move(resources)),
      tails_(resources_.size()),
      queued_(resources_.size()),
      completed_commands_(resources_.size()),
      observer_(observer) {}

std::vector<std::size_t> Timeline::insert(const CoarseGraph & graph, std::span<const std::int64_t> work_ns, std::span<const ExecutionInput> inputs,
                                          std::span<const BoundaryTime> boundaries) {
    if (work_ns.size() != graph.work.size()) throw std::invalid_argument("execution requires one given duration per work interval");
    std::vector<std::size_t> resources;
    for (const auto & resource : graph.resources) {
        const auto found = std::ranges::find(resources_, resource);
        if (found == resources_.end()) throw std::invalid_argument("action uses an undeclared execution resource");
        resources.push_back(static_cast<std::size_t>(found - resources_.begin()));
    }
    for (const auto duration : work_ns) {
        if (duration < 0) throw std::invalid_argument("given work duration must be nonnegative nanoseconds");
    }
    for (const auto & input : inputs) {
        if (!events_.contains(input.source) || input.target >= graph.events.size()) throw std::invalid_argument("undefined invocation input boundary");
    }
    std::map<std::size_t, std::int64_t> times;
    for (const auto & boundary : boundaries) {
        if (boundary.event >= graph.events.size() || boundary.time_ns < now_ns_)
            throw std::invalid_argument("boundary time refers outside this invocation or before its submission: event="
                                        + std::to_string(boundary.event) + " time_ns=" + std::to_string(boundary.time_ns)
                                        + " submitted_ns=" + std::to_string(now_ns_));
        if (!times.emplace(boundary.event, boundary.time_ns).second) throw std::invalid_argument("duplicate invocation boundary time");
    }

    std::map<std::size_t, std::vector<std::size_t>> launches;
    for (const auto & edge : graph.edges) {
        if (edge.kind == Relation::Submission) launches[edge.target].push_back(edge.source);
    }
    std::map<std::size_t, std::set<std::size_t>> finishes;
    for (const auto & work : graph.work) {
        if (resources_[resources[work.resource]].kind != ResourceKind::Stream) continue;
        if (!launches.contains(work.start)) throw std::invalid_argument("device work has no explicit submission boundary");
        finishes[work.start].insert(work.finish);
    }

    // Check execution inputs before publishing boundaries; graph construction already established structural validity.
    const auto offset = next_event_;
    std::vector<std::size_t> identities;
    for (const auto & event : graph.events) {
        identities.push_back(next_event_);
        events_.emplace(next_event_++, Event{ resources[event.resource], 0, 0, now_ns_, {}, {}, false, {}, {}, {} });
    }
    unfinished_ += graph.events.size();
    if (observer_) observer_->inserted(graph, identities, work_ns, now_ns_);
    for (const auto & [event, time_ns] : times) {
        auto & node = events_.at(offset + event);
        node.ready_ns = time_ns;
        node.latest_ns = time_ns;
        if (observer_) observer_->constrained(offset + event, time_ns, time_ns);
    }
    std::vector<bool> has_previous(graph.events.size()), has_next(graph.events.size());
    for (const auto & edge : graph.edges) {
        connect(offset + edge.source, offset + edge.target, 0, edge.kind);
        if (graph.events[edge.source].resource == graph.events[edge.target].resource) {
            has_previous[edge.target] = true;
            has_next[edge.source] = true;
        }
    }
    for (std::size_t index = 0; index < graph.work.size(); ++index) {
        const auto & work = graph.work[index];
        connect(offset + work.start, offset + work.finish, work_ns[index], Relation::Order);
        has_previous[work.finish] = true;
        has_next[work.start] = true;
    }
    SubmissionRoots origins(launches);
    for (const auto & [target, _] : launches) {
        const auto resource = resources[graph.events[target].resource];
        if (resources_[resource].kind != ResourceKind::Stream) continue;

        // Acceptance precedes asynchronous API execution; only explicit submission edges identify that queue origin.
        const auto & enqueues = origins.at(target);

        // Fences and event records are commands too, even when they carry no timed work.
        auto & completions = finishes[target];
        if (completions.empty()) completions.insert(target);
        for (const auto finish : completions) {
            const auto command = next_command_++;
            commands_.emplace(command, Command{ resource, offset + finish, enqueues.size() });
            for (const auto enqueue : enqueues) events_.at(offset + enqueue).enqueues.push_back(command);
        }
    }
    for (const auto & scope : graph.scopes) {
        for (const auto & port : scope.ports) {
            if (port.rendezvous) {
                ++events_.at(offset + port.event).pending;
                ++events_.at(offset + port.event).holds;
            }
            if (port.wait) events_.at(offset + port.wait->begin).waits.push_back({ resources[port.wait->resource], offset + port.event });
        }
    }
    for (const auto & input : inputs) connect(input.source, offset + input.target, 0, input.relation);

    // Each serial resource continues from its own preceding invocation, without an all-resource barrier.
    const auto preceding = tails_;
    for (std::size_t event = 0; event < graph.events.size(); ++event) {
        const auto resource = resources[graph.events[event].resource];
        if (!resources_[resource].serial) continue;
        if (!has_previous[event] && preceding[resource]) connect(*preceding[resource], offset + event, 0, Relation::Order);
        if (!has_next[event]) tails_[resource] = offset + event;
    }
    for (const auto event : identities) schedule(event);
    return identities;
}

void Timeline::connect(std::size_t source, std::size_t target, std::int64_t delay_ns, Relation relation) {
    auto & consumer = events_.at(target);
    if (const auto completed = events_.at(source).completed_ns) consumer.ready_ns = std::max(consumer.ready_ns, after(*completed, delay_ns));
    else {
        ++consumer.pending;
        events_.at(source).next.push_back({ target, delay_ns });
    }
    if (observer_) observer_->connected(source, target, relation, delay_ns);
}

std::size_t Timeline::defer() {
    const auto event = next_event_++;

    // A notification has causal dependencies but occupies no device queue or CPU thread.
    events_.emplace(event, Event{ {}, 1, 1, now_ns_, {}, {}, false, {}, {}, {} });
    ++unfinished_;
    return event;
}

void Timeline::resolve(std::size_t event, std::span<const std::size_t> sources, Relation relation, std::optional<TimeWindow> window) {
    auto & target = events_.at(event);
    if (target.holds == 0 || target.completed_ns) throw std::invalid_argument("communication completion is not awaiting peers");
    for (const auto source : sources) {
        if (!events_.contains(source) || source == event) throw std::invalid_argument("invalid cross-action communication input");
    }
    if (window) {
        if (window->begin_ns < 0 || window->end_ns < window->begin_ns || target.latest_ns)
            throw std::invalid_argument("boundary requires one ordered nonnegative time window");
        target.ready_ns = std::max(target.ready_ns, window->begin_ns);
        target.latest_ns = window->end_ns;
        if (observer_) observer_->constrained(event, window->begin_ns, window->end_ns);
    }
    for (const auto source : sources) connect(source, event, 0, relation);
    --target.holds;
    --target.pending;
    schedule(event);
}

void Timeline::schedule(std::size_t event) {
    auto & node = events_.at(event);
    if (node.pending != 0 || node.completed_ns) return;

    // A late-resolved external hold cannot schedule a boundary in the executor's past.
    node.ready_ns = std::max(node.ready_ns, now_ns_);

    // Measurements constrain time, never waive dependencies or turn a late boundary into a shifted success.
    if (node.latest_ns && node.ready_ns > *node.latest_ns)
        throw std::runtime_error("boundary " + std::to_string(event) + " requires " + std::to_string(*node.latest_ns) + " ns but dependencies permit "
                                 + std::to_string(node.ready_ns) + " ns");
    ready_.emplace(node.ready_ns, event);
}

std::optional<EventCompletion> Timeline::advance(std::optional<std::int64_t> until_ns) {
    if (until_ns && *until_ns < now_ns_) throw std::invalid_argument("execution cannot move backwards to an arrival");
    while (!ready_.empty()) {
        const auto [time_ns, identity] = ready_.top();
        if (until_ns && time_ns > *until_ns) {
            now_ns_ = *until_ns;
            return std::nullopt;
        }
        ready_.pop();
        const auto found = events_.find(identity);
        if (found == events_.end()) continue;
        auto & event = found->second;
        if (event.completed_ns || event.pending != 0 || event.ready_ns != time_ns) continue;
        now_ns_ = time_ns;
        if (!event.activated) {
            event.activated = true;
            for (const auto command_id : event.enqueues) {
                auto & command = commands_.at(command_id);
                if (--command.pending == 0) {
                    queued_[command.resource].insert(command.finish);
                    commands_.erase(command_id);
                }
            }
            event.enqueues.clear();

            // Waiting includes queued work and the completed frontier, but never a later enqueue.
            for (const auto & wait : event.waits) {
                if (const auto completed = completed_commands_[wait.resource]) connect(*completed, wait.finish, 0, Relation::Signal);
                for (const auto completion : queued_[wait.resource]) connect(completion, wait.finish, 0, Relation::Signal);
                schedule(wait.finish);
            }
            event.waits.clear();
            if (event.pending != 0) continue;
        }

        event.completed_ns = time_ns;
        --unfinished_;
        if (event.resource && queued_[*event.resource].erase(identity)) completed_commands_[*event.resource] = identity;
        for (const auto & edge : event.next) {
            auto & target = events_.at(edge.target);
            target.ready_ns = std::max(target.ready_ns, after(time_ns, edge.delay_ns));
            --target.pending;
            schedule(edge.target);
        }
        event.next.clear();
        if (observer_) observer_->completed(identity, time_ns);
        return EventCompletion{ identity, time_ns };
    }
    if (until_ns) now_ns_ = *until_ns;
    return std::nullopt;
}

bool Timeline::completed(std::size_t event) const { return events_.at(event).completed_ns.has_value(); }

void Timeline::retain(std::set<std::size_t> external) {
    for (const auto tail : tails_) {
        if (tail) external.insert(*tail);
    }
    for (const auto completion : completed_commands_) {
        if (completion) external.insert(*completion);
    }

    // Owners keep data and unmatched protocol boundaries; unfinished events still own their outgoing dependencies.
    std::erase_if(events_, [&](const auto & entry) { return entry.second.completed_ns && !external.contains(entry.first); });
}

bool Timeline::done() const { return unfinished_ == 0; }
std::int64_t Timeline::now_ns() const { return now_ns_; }

} // namespace markov
