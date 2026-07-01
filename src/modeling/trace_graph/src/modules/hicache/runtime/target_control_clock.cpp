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

/** @brief trace finalize 是 target-derived 的 terminal boundary，用于收敛残留 async lifecycle。 */
HiCacheControlBoundary HiCacheTargetControlClock::record_target_finalize_boundary(const std::string & cache_scope, uint64_t ts) {
    return record_boundary(cache_scope, "", "finalize", "target_finalize", 0, ts, true);
}

/**
 * @brief 统一登记 target control boundary。
 *
 * scheduler_epoch 和 boundary_epoch 都在这里分配，保证 target boundary/finalize 的
 * 顺序可以在 summary 中复现。
 */
HiCacheControlBoundary HiCacheTargetControlClock::record_boundary(const std::string & cache_scope, const std::string & request_key, const std::string & kind,
                                                                  const std::string & source, size_t source_event_index, uint64_t ts, bool terminal) {
    HiCacheControlBoundary boundary{
        .scheduler_epoch = ++scheduler_epoch_,
        .boundary_epoch = ++boundary_epoch_,
        .cache_scope = cache_scope,
        .request_key = request_key,
        .kind = kind,
        .source = source,
        .source_event_index = source_event_index,
        .ts = ts,
        .terminal = terminal,
    };
#ifdef DEBUG
    boundaries_.push_back(boundary);
#endif
    return boundary;
}

} // namespace markov::trace_graph::modules::hicache::runtime
