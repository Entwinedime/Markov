#include "framework/symbol_resolver.hpp"
#include <cstdlib>
#include <dlfcn.h>
#include <iostream>
#include <mutex>

namespace HookFrameWork {

void * ResolveSymbol(const std::string & mangled_name, const std::string & so_path) {
    /**
     * @brief dlfcn 解析过程串行化，避免多个 wrapper 首次调用时同时 dlopen/dlsym 同一目标库。
     */
    static std::mutex resolve_mtx;
    std::lock_guard<std::mutex> lock(resolve_mtx);

    dlerror();
    void * target_handle = dlopen(so_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!target_handle) { std::cerr << "[hook] dlopen target failed: " << dlerror() << std::endl; }

    void * symbol = nullptr;
    if (target_handle) { symbol = dlsym(target_handle, mangled_name.c_str()); }

    if (!symbol) {
        /**
         * @brief 原符号解析失败属于采集配置硬错误。
         *
         * 继续运行会导致 wrapper 无法调用真实函数，因此直接退出而不是产出不完整 trace。
         */
        std::cerr << "[hook] FATAL: Failed to resolve symbol: " << mangled_name << std::endl;
        std::exit(EXIT_FAILURE);
    }

    return symbol;
}

} // namespace HookFrameWork
