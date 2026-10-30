#include "markov/resources/trace.hpp"

#include <utility>

namespace markov {

void ResourceTrace::context(std::int64_t time_ns, std::size_t order, std::string_view phase, std::optional<TaskIdentity> task) {
    time_ns_ = time_ns;
    order_ = order;
    phase_ = phase;
    task_ = std::move(task);
}

void ResourceTrace::append(std::string_view pool, std::string_view kind, nlohmann::json facts) {
    facts["pool"] = pool;
    facts["kind"] = kind;
    facts["time_ns"] = time_ns_;
    facts["order"] = order_;
    facts["phase"] = phase_;
    if (task_)
        facts["task"] = {
            {    "group",    task_->group },
            { "sequence", task_->sequence }
        };
    events_.push_back(std::move(facts));
}

nlohmann::json ResourceTrace::take() { return std::move(events_); }

} // namespace markov
