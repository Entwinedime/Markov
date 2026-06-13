/**
 * @file
 * @brief SGLang runtime hook profile。
 *
 * 当前 active LD_PRELOAD 框架按 C++ wrapper 硬编码拦截目标符号。SGLang profile
 * 先复用 AscendCL runtime 同步点；后续若要采集 SGLang / HiCache 的 native 事件，
 * 必须在这里新增明确签名、明确符号名、明确目标 so 的 wrapper，不能从 Python 配置动态生成。
 */
#include "ascendcl_hooks.cpp"
