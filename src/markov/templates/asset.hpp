#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/templates/catalog.hpp"
#include "markov/templates/group_execution.hpp"

namespace markov {

class LayoutMismatch : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct BoundExecution {
    std::size_t pattern;
    std::vector<std::size_t> resources;
    std::optional<GroupExecution> reduction;
};

struct ExecutionVariant {
    std::vector<LayoutGuard> guards;
    BoundExecution binding;
};

struct ExecutionSelection {
    std::size_t definition;
    std::size_t operation;
    std::vector<ExecutionVariant> choices;
};

using ExecutionBinding = std::variant<BoundExecution, std::size_t>;

// Session bindings are outside shared identity and do not supply target dynamic defaults.
struct FixedBindings {
    Parameters values;
    std::vector<nlohmann::json> literals;
    std::optional<std::size_t> host_resource;
    std::map<std::size_t, std::size_t> external_resources;
    std::map<std::size_t, ExecutionBinding> execution;

    std::vector<FixedBindings> children;
    std::map<std::string, FixedBindings> producers;
};

struct TemplateEntry {
    std::size_t layout;
    FixedBindings bindings;
};

struct RegionSelection {
    std::string role;
    std::optional<std::string> phase;
    std::size_t entry;
    std::vector<std::size_t> path;
};

// A resolved source call borrows its immutable bindings from the task's asset.
struct TemplateFrame {
    std::size_t layout;
    const FixedBindings & bindings;
};

// Parse role-local bindings at this boundary; expansion consumes the shared typed catalog.
class TemplateAsset {
public:
    TemplateAsset(const TemplateCatalog & catalog, nlohmann::json document);
    static std::vector<TemplateAsset> read_roles(const TemplateCatalog & catalog, const nlohmann::json & roles);

    const TemplateDefinition & definition(std::size_t identity) const;
    const TemplateLayout & layout(std::size_t identity) const;
    const TemplateEntry & entry(std::size_t identity) const;
    TemplateFrame frame(const RegionSelection & selection) const;

    const nlohmann::json & environment() const;
    const nlohmann::json & physical_coverage() const;
    const nlohmann::json & runtime_coverage() const;

    const std::vector<std::string> & execution_resources() const;
    const ExecutionPattern & execution_pattern(std::size_t identity) const;
    const BoundExecution & execution_binding(const ExecutionBinding & binding, const Parameters & parameters) const;
    const std::vector<RegionSelection> & entrypoints() const;

private:
    enum class CallVisit { Unseen, Active, Complete };

    TemplateAsset bind_role(const nlohmann::json & instance) const;
    FixedBindings fixed_bindings(const nlohmann::json & source) const;
    BoundExecution bound_execution(const nlohmann::json & source) const;
    void validate_bindings(std::size_t layout, const FixedBindings & bindings) const;
    void validate_calls(std::size_t layout, std::vector<CallVisit> & visiting) const;

    const TemplateCatalog & catalog_;
    nlohmann::json environment_;
    nlohmann::json physical_coverage_;
    nlohmann::json runtime_coverage_;
    std::vector<std::string> execution_resources_;
    std::vector<ExecutionSelection> execution_variants_;

    std::vector<TemplateEntry> entries_;
    std::vector<RegionSelection> entrypoints_;
};

} // namespace markov
