#include "framework/symbol_resolver.hpp"
#include <cstdlib>
#include <dlfcn.h>
#include <iostream>
#include <mutex>

namespace HookFrameWork {

void * ResolveSymbol(const std::string & mangled_name, const std::string & so_path) {
    static std::mutex resolve_mtx;
    std::lock_guard<std::mutex> lock(resolve_mtx);

    dlerror();
    void * target_handle = dlopen(so_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!target_handle) { std::cerr << "[hook] dlopen target failed: " << dlerror() << std::endl; }

    void * symbol = nullptr;
    if (target_handle) { symbol = dlsym(target_handle, mangled_name.c_str()); }

    if (!symbol) {
        std::cerr << "[hook] FATAL: Failed to resolve symbol: " << mangled_name << std::endl;
        std::exit(EXIT_FAILURE);
    }

    return symbol;
}

} // namespace HookFrameWork
