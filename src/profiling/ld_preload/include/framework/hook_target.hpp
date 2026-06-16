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

/**
 * @brief 单个 LD_PRELOAD 目标函数的惰性解析器。
 *
 * HookTarget 保存 trace 名称、目标符号和目标 so 路径；第一次调用时解析原函数并注册函数范围，
 * 后续 wrapper 通过 Original() 调用真实实现。
 */
template <typename FnType> class HookTarget {
public:
    HookTarget(const std::string & trace_name, const std::string & mangled_name, const std::string & so_path)
        : trace_name_(trace_name),
          mangled_name_(mangled_name),
          so_path_(so_path) {}

    /** @brief 返回真实函数指针；首次调用会解析目标符号。 */
    FnType Original() {
        EnsureInitialized();
        return original_;
    }

    /** @brief Chrome trace 中使用的稳定事件名。 */
    const std::string & TraceName() const { return trace_name_; }

    /** @brief 返回真实函数范围；首次调用会触发符号解析。 */
    const FunctionScope & Scope() {
        EnsureInitialized();
        return scope_;
    }

private:
    void EnsureInitialized() {
        std::call_once(init_once_, [this]() { Initialize(); });
    }

    void Initialize() {
        /**
         * @brief 初始化阶段必须同时解析真实符号和注册 scope。
         *
         * RelationRules 依赖 scope 判断调用关系；如果只解析 original_ 而不注册 scope，
         * caller 过滤会退化为无法匹配。
         */
        void * symbol_addr{ ResolveSymbol(mangled_name_, so_path_) };
        original_ = reinterpret_cast<FnType>(symbol_addr);

        scope_ = BuildScope(symbol_addr);

        ScopeRegistry::Get().RegisterScope(trace_name_, scope_);
    }

    static FunctionScope BuildScope(void * symbol_addr) {
        /**
         * @brief 从动态链接器提供的符号元数据推导函数范围。
         *
         * 部分平台或符号缺少 st_size，这时 end 保持 0，RelationRules 不会把该 scope 当作可匹配范围。
         */
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
