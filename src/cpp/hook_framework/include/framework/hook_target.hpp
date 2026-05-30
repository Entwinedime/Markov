#pragma once

#include "framework/scope_registry.hpp"
#include "framework/symbol_resolver.hpp"
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <mutex>
#include <stdint.h>
#include <string>

namespace HookFrameWork {

template <typename FnType> class HookTarget {
public:
    HookTarget(const std::string & trace_name, const std::string & mangled_name, const std::string & so_path)
        : trace_name_(trace_name),
          mangled_name_(mangled_name),
          so_path_(so_path) {}

    FnType Original() {
        EnsureInitialized();
        return original_;
    }

    const std::string & TraceName() const { return trace_name_; }

    const FunctionScope & Scope() {
        EnsureInitialized();
        return scope_;
    }

private:
    void EnsureInitialized() {
        std::call_once(init_once_, [this]() { Initialize(); });
    }

    void Initialize() {
        void * symbol_addr{ ResolveSymbol(mangled_name_, so_path_) };
        original_ = reinterpret_cast<FnType>(symbol_addr);

        scope_ = BuildScope(symbol_addr);

        ScopeRegistry::Get().RegisterScope(trace_name_, scope_);
    }

    static FunctionScope BuildScope(void * symbol_addr) {
        FunctionScope scope{ reinterpret_cast<uintptr_t>(symbol_addr), 0 };

#if defined(__GLIBC__) && defined(_GNU_SOURCE)
        Dl_info info;
        std::memset(&info, 0, sizeof(info));
        void * sym_extra{ nullptr };
        if (symbol_addr != nullptr && dladdr1(symbol_addr, &info, &sym_extra, RTLD_DL_SYMENT) != 0 && sym_extra != nullptr) {
            const ElfW(Sym) * sym{ reinterpret_cast<const ElfW(Sym) *>(sym_extra) };
            if (sym->st_size > 0) { scope.end = scope.begin + static_cast<uintptr_t>(sym->st_size); }
        }
#endif

        return scope;
    }

    const std::string trace_name_;
    const std::string mangled_name_;
    const std::string so_path_;
    std::once_flag init_once_;
    FnType original_{ nullptr };
    FunctionScope scope_;
};

} // namespace HookFrameWork
