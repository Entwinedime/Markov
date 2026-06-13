#pragma once

#include <initializer_list>
#include <map>
#include <mutex>
#include <stdint.h>
#include <string>
#include <vector>

namespace HookFrameWork {

/**
 * @brief wrapper 间调用关系过滤规则。
 *
 * Profiling 只采事实；RelationRules 只决定某个 callee 是否在指定 caller 范围内被记录，
 * 不推导 target policy 或建模行为。
 */
class RelationRules {
  public:
    static RelationRules & Get();

    /** @brief 覆盖某个 callee 的允许 caller 列表；空列表表示不限制 caller。 */
    void UpsertRule(const std::string & callee, const std::vector<std::string> & callers);
    /** @brief initializer_list 便捷重载，忽略空 caller 名称。 */
    void UpsertRule(const std::string & callee, std::initializer_list<std::string> callers);
    /** @brief 根据 callee 名称和调用方 PC 判断本次调用是否应该写 trace。 */
    bool ShouldTrace(const std::string & callee_name, uintptr_t caller_pc) const;

  private:
    RelationRules();

    mutable std::mutex mtx_;
    std::map<std::string, std::vector<std::string>> rules_;
};

} // namespace HookFrameWork
