#include "framework/relation_rules.hpp"
#include "framework/scope_registry.hpp"
#include <cstdio>
#include <dlfcn.h>

namespace HookFrameWork {

RelationRules & RelationRules::Get() {
    static RelationRules instance;
    return instance;
}

RelationRules::RelationRules() {}


void RelationRules::UpsertRule(const std::string & callee, const std::vector<std::string> & callers) {
    if (callee.empty()) { return; }

    std::lock_guard<std::mutex> lock(mtx_);
    rules_[callee] = callers;
}

void RelationRules::UpsertRule(const std::string & callee, std::initializer_list<std::string> callers) {
    std::vector<std::string> caller_list;
    caller_list.reserve(callers.size());
    for (std::initializer_list<std::string>::const_iterator it = callers.begin(); it != callers.end(); ++it) {
        if (!it->empty()) { caller_list.push_back(*it); }
    }

    UpsertRule(callee, caller_list);
}

bool RelationRules::ShouldTrace(const std::string & callee_name, uintptr_t caller_pc) const {
    std::vector<std::string> callers;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        std::map<std::string, std::vector<std::string>>::const_iterator it{ rules_.find(callee_name) };
        if (it == rules_.end()) { return true; }
        callers = it->second;
    }

    if (callers.empty()) { return true; }
    if (caller_pc == 0) { return false; }
    for (size_t i = 0; i < callers.size(); ++i) {
        if (ScopeRegistry::Get().ContainsInScope(callers[i], caller_pc)) { return true; }
    }

    return false;
}
} // namespace HookFrameWork
