#pragma once

#include "markov/trace_graph/structure/operation.hpp"
#include <array>
#include <string_view>

namespace markov::trace_graph::modules::hicache::model {

enum class PrefetchSchedulerAction { Receive, WriteCompletion, LoadCompletion, StorageDrain };

inline constexpr std::array<std::string_view, 4> PREFETCH_SCHEDULER_NAMES{ "receive", "write_completion", "load_completion", "storage_drain" };

// SGLang's serial scheduler receives requests before get_next_batch_to_run;
// HiCache then checks writes, loads and storage control in this order. Costs
// bind to these actions, rather than declaring a program in a sample file.
inline constexpr std::array PREFETCH_SCHEDULER_SEQUENCE{
    PrefetchSchedulerAction::Receive,
    PrefetchSchedulerAction::WriteCompletion,
    PrefetchSchedulerAction::LoadCompletion,
    PrefetchSchedulerAction::StorageDrain,
};

} // namespace markov::trace_graph::modules::hicache::model

namespace markov::trace_graph::structure::hicache {

// Local roles are action.before/action.after or tail. Communication roles are
// action.collective; scopes distinguish service from residual/dispatch time.
[[nodiscard]] OperationWork prefetch_scheduler_operation(std::string_view role, std::string_view scope, int rank, size_t participants);

} // namespace markov::trace_graph::structure::hicache
