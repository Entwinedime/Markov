#pragma once

#include <mutex>
#include <stdint.h>
#include <string>
#include <vector>

namespace HookFrameWork {

struct FunctionScope {
    uintptr_t begin{ 0 };
    uintptr_t end{ 0 };

    bool Contains(uintptr_t pc) const { return begin != 0 && end > begin && pc >= begin && pc < end; }
};

class ScopeRegistry {
public:
    static ScopeRegistry & Get();

    void RegisterScope(const std::string & scope_name, const FunctionScope & scope);
    bool Contains(uintptr_t pc) const;
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
