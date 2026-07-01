/**
 * @file
 * @brief HiCache target-control boundary 的逻辑时钟。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

/**
 * @brief target control clock 中的一次 boundary 记录。
 *
 * boundary 由 target model 生成，只描述 target state model 的推进顺序，不代表
 * source actual completion。
 */
struct HiCacheControlBoundary {
    uint64_t scheduler_epoch = 0;
    uint64_t boundary_epoch = 0;
    std::string cache_scope;
    std::string request_key;
    std::string kind;
    std::string source;
    size_t source_event_index = 0;
    uint64_t ts = 0;
    bool terminal = false;
};

/**
 * @brief HiCache target-derived control clock 的统一封装。
 *
 * 该 clock 统一生成 operation id、enqueue epoch 和 boundary epoch。当前模型仍然把
 * ack 时序同步折叠，但不再把 control-flow clock 分散在 async operation table 里。
 */
class HiCacheTargetControlClock {
public:
    /** @brief 生成 target operation id。 */
    [[nodiscard]] std::string next_operation_id(std::string_view kind);

    /** @brief 生成 enqueue epoch，并推进 scheduler epoch。 */
    [[nodiscard]] uint64_t next_enqueue_epoch();

    /** @brief 记录 target finalize 生成的 boundary。 */
    [[nodiscard]] HiCacheControlBoundary record_target_finalize_boundary(const std::string & cache_scope, uint64_t ts);

#ifdef DEBUG
    /** @brief 所有 modeled boundary。 */
    [[nodiscard]] const std::vector<HiCacheControlBoundary> & boundaries() const { return boundaries_; }
#endif

    /** @brief 当前 boundary 数。 */
    [[nodiscard]] uint64_t boundary_count() const { return boundary_epoch_; }

private:
    uint64_t scheduler_epoch_ = 0;
    uint64_t enqueue_epoch_ = 0;
    uint64_t boundary_epoch_ = 0;
    uint64_t operation_epoch_ = 0;
#ifdef DEBUG
    std::vector<HiCacheControlBoundary> boundaries_;
#endif

    [[nodiscard]] HiCacheControlBoundary record_boundary(const std::string & cache_scope, const std::string & request_key, const std::string & kind,
                                                         const std::string & source, size_t source_event_index, uint64_t ts, bool terminal);
};

} // namespace markov::trace_graph::modules::hicache::runtime
