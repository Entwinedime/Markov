#pragma once

#include <initializer_list>
#include <map>
#include <mutex>
#include <stdint.h>
#include <string>
#include <vector>

namespace HookFrameWork {

class RelationRules {
public:
    static RelationRules & Get();

    void UpsertRule(const std::string & callee, const std::vector<std::string> & callers);
    void UpsertRule(const std::string & callee, std::initializer_list<std::string> callers);
    bool ShouldTrace(const std::string & callee_name, uintptr_t caller_pc) const;

private:
    RelationRules();

    mutable std::mutex mtx_;
    std::map<std::string, std::vector<std::string>> rules_;
};

} // namespace HookFrameWork
