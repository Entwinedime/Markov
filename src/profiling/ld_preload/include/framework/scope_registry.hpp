#pragma once

#include <mutex>
#include <stdint.h>
#include <string>
#include <vector>

namespace HookFrameWork {

/** @brief 已解析目标函数在进程地址空间中的半开区间。 */
struct FunctionScope {
    uintptr_t begin{0};
    uintptr_t end{0};

    /** @brief 判断 pc 是否落在函数范围内；缺失 size 的符号不会匹配。 */
    bool Contains(uintptr_t pc) const { return begin != 0 && end > begin && pc >= begin && pc < end; }
};

/**
 * @brief 记录已解析 hook target 的函数地址范围。
 *
 * RelationRules 通过这个 registry 判断当前调用方是否属于允许采集的上游函数。
 */
class ScopeRegistry {
  public:
    static ScopeRegistry & Get();

    /** @brief 注册或更新一个命名函数范围。 */
    void RegisterScope(const std::string & scope_name, const FunctionScope & scope);
    /** @brief 判断 pc 是否落在任意已注册范围内。 */
    bool Contains(uintptr_t pc) const;
    /** @brief 判断 pc 是否落在指定命名范围内。 */
    bool ContainsInScope(const std::string & scope_name, uintptr_t pc) const;

  private:
    struct ScopeEntry {
        std::string scope_name;
        FunctionScope scope;
    };

    mutable std::mutex mtx_;
    std::vector<ScopeEntry> entries_;
};

} // namespace HookFrameWork
