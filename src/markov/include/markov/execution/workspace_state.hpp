#pragma once

#include "markov/templates/templates.hpp"

#include <cstdint>
#include <expected>
#include <map>
#include <span>
#include <string>

namespace markov {

// One simulation owns these capacities. Advance a staged copy when admitting
// ordered work; rescheduling the execution graph never reapplies allocations.
class WorkspaceState {
public:
    explicit WorkspaceState(std::span<const TemplateWorkspacePool> pools);
    std::expected<void, std::string> advance(TemplateInstance & instance, const TemplateBank & templates, const BatchInput & input);
    const std::map<TemplateResource, std::uint64_t> & capacities() const { return capacities_; }

private:
    std::map<TemplateResource, std::uint64_t> capacities_;
};

} // namespace markov
