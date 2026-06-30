/**
 * @file
 * @brief HiCache target control clock 实现。
 */
#include "markov/trace_graph/modules/hicache/runtime/target_control_clock.hpp"

#include <sstream>

namespace markov::trace_graph::modules::hicache::runtime {

/** @brief 为 target-derived async operation 分配当前 scope 内可读的单调 id。 */
std::string HiCacheTargetControlClock::next_operation_id(std::string_view kind) {
    std::ostringstream os;
    os << kind << ":" << ++operation_epoch_;
    return os.str();
}

/**
 * @brief enqueue epoch 与 scheduler epoch 同步推进。
 *
 * 这里的时钟只服务 target model 中的因果解释，不代表 source runtime 的真实线程调度时间。
 */
uint64_t HiCacheTargetControlClock::next_enqueue_epoch() {
    ++scheduler_epoch_;
    return ++enqueue_epoch_;
}

/** @brief 用 source checkpoint anchor 记录 target scheduler 可见的 prefetch 检查点。 */
HiCacheControlCheckpoint HiCacheTargetControlClock::record_target_checkpoint(const std::string & cache_scope, const std::string & request_key, uint64_t ts,
                                                                             bool terminal, size_t source_event_index) {
    return record_checkpoint(cache_scope, request_key, "prefetch_check_point", "target_scheduler_from_source_anchor", source_event_index, ts, terminal);
}

/** @brief trace finalize 是 target-derived 的 terminal checkpoint，用于收敛残留 async lifecycle。 */
HiCacheControlCheckpoint HiCacheTargetControlClock::record_target_finalize_checkpoint(const std::string & cache_scope, uint64_t ts) {
    return record_checkpoint(cache_scope, "", "finalize", "target_finalize", 0, ts, true);
}

/**
 * @brief 统一登记 target control checkpoint。
 *
 * scheduler_epoch 和 checkpoint_epoch 都在这里分配，保证 prefetch/checkpoint/finalize
 * 的顺序可以在 summary 中复现。
 */
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
