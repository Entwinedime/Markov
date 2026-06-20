#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace TraceGraph {

/**
 * @brief target control clock 中的一次 checkpoint 记录。
 *
 * checkpoint 由 target scheduler 生成；source invariant checkpoint 只作为触发锚点。
 * 该记录只描述 target state model 的推进顺序，不代表 source actual completion。
 */
struct HiCacheControlCheckpoint {
    uint64_t scheduler_epoch = 0;
    uint64_t checkpoint_epoch = 0;
    std::string cache_scope;
    std::string request_key;
    std::string kind;
    std::string source;
    size_t source_event_index = 0;
    uint64_t ts = 0;
    bool terminal = false;
};

/**
 * @brief HiCache target-derived control clock。
 *
 * 该 clock 统一生成 operation id、enqueue epoch 和 checkpoint epoch。当前模型仍然把
 * ack 时序同步折叠，但不再把 control-flow clock 分散在 async operation table 里。
 */
class HiCacheTargetControlClock {
public:
    /** @brief 生成 target operation id。 */
    [[nodiscard]] std::string next_operation_id(std::string_view kind);

    /** @brief 生成 enqueue epoch，并推进 scheduler epoch。 */
    [[nodiscard]] uint64_t next_enqueue_epoch();

    /** @brief 记录由 source invariant anchor 触发的 target scheduler checkpoint。 */
    [[nodiscard]] HiCacheControlCheckpoint record_target_checkpoint(const std::string & cache_scope, const std::string & request_key,
                                                                    const std::string & check_kind, uint64_t ts, bool terminal, size_t source_event_index);

    /** @brief 记录 target finalize 生成的 checkpoint。 */
    [[nodiscard]] HiCacheControlCheckpoint record_target_finalize_checkpoint(const std::string & cache_scope, uint64_t ts);

    /** @brief 所有 modeled checkpoint。 */
    [[nodiscard]] const std::vector<HiCacheControlCheckpoint> & checkpoints() const { return checkpoints_; }

    /** @brief 当前 checkpoint 数。 */
    [[nodiscard]] uint64_t checkpoint_count() const { return static_cast<uint64_t>(checkpoints_.size()); }

private:
    uint64_t scheduler_epoch_ = 0;
    uint64_t enqueue_epoch_ = 0;
    uint64_t checkpoint_epoch_ = 0;
    uint64_t operation_epoch_ = 0;
    std::vector<HiCacheControlCheckpoint> checkpoints_;

    [[nodiscard]] HiCacheControlCheckpoint record_checkpoint(const std::string & cache_scope, const std::string & request_key, const std::string & kind,
                                                             const std::string & source, size_t source_event_index, uint64_t ts, bool terminal);
};

} // namespace TraceGraph
