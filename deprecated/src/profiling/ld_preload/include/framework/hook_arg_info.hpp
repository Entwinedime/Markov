#pragma once

#include <string>

namespace HookFrameWork {

/** @brief 单个 wrapper 参数的 trace 输出描述。 */
struct HookArgInfo {
    /** @brief trace 中展示的参数名。 */
    const std::string name;
    /** @brief 已经由 wrapper 显式转换好的字符串值。 */
    const std::string value_in_string;
};

} // namespace HookFrameWork
