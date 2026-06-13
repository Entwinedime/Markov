#pragma once

#include <string>
#include <vector>

namespace HookFrameWork {

/** @brief 一次 PMU 计数器读数快照。 */
struct PmuSnapshot {
    bool valid = false;
    std::vector<std::string> event_names;
    std::vector<long long> counter_values;

    /** @brief 只有事件名和计数器数量一致的快照才能做 delta。 */
    bool IsCompatibleWith(const PmuSnapshot & begin) const {
        return valid && begin.valid && event_names == begin.event_names && counter_values.size() == begin.counter_values.size();
    }

    /** @brief 计算当前快照相对 begin 的计数器增量；不兼容时返回空数组。 */
    std::vector<long long> DeltaFrom(const PmuSnapshot & begin) const {
        std::vector<long long> deltas;
        if (!IsCompatibleWith(begin)) { return deltas; }

        deltas.reserve(counter_values.size());
        for (size_t i = 0; i < counter_values.size(); ++i) { deltas.push_back(counter_values[i] - begin.counter_values[i]); }
        return deltas;
    }
};

/**
 * @brief 可选 PMU 采集入口。
 *
 * 未启用 PAPI 或当前线程初始化失败时返回 false；调用方应继续输出基础 runtime trace。
 */
class PmuRecorder {
  public:
    static PmuRecorder & Get();
    /** @brief 读取当前线程 PMU 快照，失败时不修改建模语义。 */
    bool ReadSnapshot(PmuSnapshot * snapshot);

  private:
    PmuRecorder() = default;
};

} // namespace HookFrameWork
