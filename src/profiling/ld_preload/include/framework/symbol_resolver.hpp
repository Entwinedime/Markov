#pragma once

#include <string>

namespace HookFrameWork {

/**
 * @brief 从目标 so 中解析原始符号地址。
 *
 * LD_PRELOAD wrapper 必须调用真实实现；解析失败时进程直接退出，避免继续运行但 trace 已经缺失关键事实。
 */
void * ResolveSymbol(const std::string & mangled_name, const std::string & so_path);

} // namespace HookFrameWork
