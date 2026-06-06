#include "framework/scope_registry.hpp"

namespace HookFrameWork {

ScopeRegistry & ScopeRegistry::Get() {
    static ScopeRegistry instance;
    return instance;
}

void ScopeRegistry::RegisterScope(const std::string & scope_name, const FunctionScope & scope) {
    if (scope_name.empty() || !scope.Contains(scope.begin)) { return; }

    std::lock_guard<std::mutex> lock(mtx_);
    for (ScopeEntry & entry : entries_) {
        if (entry.scope_name == scope_name) {
            entry.scope = scope;
            return;
        }
    }

    ScopeEntry entry{ scope_name, scope };
    entries_.push_back(entry);
}

bool ScopeRegistry::Contains(uintptr_t pc) const {
    std::lock_guard<std::mutex> lock(mtx_);
    for (const ScopeEntry & entry : entries_) {
        if (entry.scope.Contains(pc)) { return true; }
    }
    return false;
}

bool ScopeRegistry::ContainsInScope(const std::string & scope_name, uintptr_t pc) const {
    std::lock_guard<std::mutex> lock(mtx_);
    for (const ScopeEntry & entry : entries_) {
        if (entry.scope_name == scope_name && entry.scope.Contains(pc)) { return true; }
    }
    return false;
}

} // namespace HookFrameWork
