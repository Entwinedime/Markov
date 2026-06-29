/**
 * @file
 * @brief HiCache target control clock 实现。
 */
#include "markov/trace_graph/modules/hicache/runtime/target_control_clock.hpp"

#include <sstream>

namespace markov::trace_graph::modules::hicache::runtime {

std::string HiCacheTargetControlClock::next_operation_id(std::string_view kind) {
    std::ostringstream os;
    os << kind << ":" << ++operation_epoch_;
    return os.str();
}

uint64_t HiCacheTargetControlClock::next_enqueue_epoch() {
    ++scheduler_epoch_;
    return ++enqueue_epoch_;
}

HiCacheControlCheckpoint HiCacheTargetControlClock::record_target_checkpoint(const std::string & cache_scope, const std::string & request_key, uint64_t ts,
                                                                             bool terminal, size_t source_event_index) {
    return record_checkpoint(cache_scope, request_key, "prefetch_check_point", "target_scheduler_from_source_anchor", source_event_index, ts, terminal);
}

HiCacheControlCheckpoint HiCacheTargetControlClock::record_target_finalize_checkpoint(const std::string & cache_scope, uint64_t ts) {
    return record_checkpoint(cache_scope, "", "finalize", "target_finalize", 0, ts, true);
}

HiCacheControlCheckpoint HiCacheTargetControlClock::record_checkpoint(const std::string & cache_scope, const std::string & request_key,
                                                                      const std::string & kind, const std::string & source, size_t source_event_index,
                                                                      uint64_t ts, bool terminal) {
    HiCacheControlCheckpoint checkpoint{
        .scheduler_epoch = ++scheduler_epoch_,
        .checkpoint_epoch = ++checkpoint_epoch_,
        .cache_scope = cache_scope,
        .request_key = request_key,
        .kind = kind,
        .source = source,
        .source_event_index = source_event_index,
        .ts = ts,
        .terminal = terminal,
    };
    checkpoints_.push_back(checkpoint);
    return checkpoint;
}

} // namespace markov::trace_graph::modules::hicache::runtime
