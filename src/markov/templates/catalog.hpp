#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/contracts/expression.hpp"
#include "markov/contracts/input_group.hpp"
#include "markov/execution/protocol.hpp"
#include "markov/graph/access.hpp"

namespace markov {

// Compile parameter and value references once; instantiate without reparsing the argument tree.
class ArgumentTree {
public:
    explicit ArgumentTree(nlohmann::json source);
    nlohmann::json instantiate(std::span<const std::size_t> values, const Parameters & parameters, const GroupValues & groups,
                               const std::vector<nlohmann::json> & literals) const;

private:
    void compile(const nlohmann::json & source, const nlohmann::json::json_pointer & path);

    nlohmann::json source_;
    std::vector<std::pair<nlohmann::json::json_pointer, std::size_t>> references_;
    std::vector<std::pair<nlohmann::json::json_pointer, std::size_t>> literal_references_;
    std::vector<std::pair<nlohmann::json::json_pointer, std::vector<Expression>>> shapes_;
    std::vector<std::pair<nlohmann::json::json_pointer, Expression>> scalars_;
    std::vector<std::pair<nlohmann::json::json_pointer, std::string>> groups_;
};

enum class ValueKind { Tensor, Resource };

struct TemplateValue {
    std::size_t storage;
    ValueKind kind;
    std::string dtype;
    std::string device;
    std::vector<Expression> shape;
};

struct TemplateOperation {
    Operation access;
    ArgumentTree arguments;
    ArgumentTree keywords;
    nlohmann::json properties;
    std::vector<std::string> input_groups;
    std::optional<ArgumentTree> communication;
};

struct LayoutGuard {
    Expression condition;
    bool expected;
};

struct StateEffect {
    std::string resource;
    Expression before;
    Expression after;
};

// A shared definition owns only its region's work, independent of child call count.
struct TemplateDefinition {
    std::string role;
    std::optional<std::string> type;
    bool dynamic_calls = false;
    std::optional<std::string> call_role;
    std::optional<Expression> call_count;
    std::vector<TemplateValue> values;
    std::vector<TemplateOperation> operations;

    std::vector<std::string> parameters;
    std::vector<std::string> bindings;
    std::vector<LayoutGuard> guards;
    ParameterRelations relations;

    std::vector<StateEffect> state_effects;
    std::map<std::string, InputGroup> input_groups;
    std::size_t resources;

    bool host_only() const;
};

struct OperationStep {
    std::size_t operation;
};

struct ReusedGroupValue {
    std::size_t value;
};

struct GroupProducer {
    std::size_t layout;
};

struct GeneratedGroup {
    Expression count;
    std::variant<ReusedGroupValue, GroupProducer> element;
};

struct MappedGroup {
    std::string group;
    std::string family;
    std::string input;
    std::string output;
};

struct CallStep {
    std::size_t layout;
    std::vector<std::size_t> inputs;
    std::vector<std::size_t> outputs;
    std::vector<std::size_t> resources;
    std::optional<std::string> family;
    std::map<std::string, std::variant<std::vector<std::size_t>, std::string, GeneratedGroup, MappedGroup>> input_groups;
    std::map<std::string, Expression> parameters;
};

struct ShapeContract {
    std::size_t value;
    std::string dtype;
    std::string device;
    std::vector<Expression> shape;
};

// Layouts compose child calls and bind aliases without duplicating shared definitions.
struct TemplateLayout {
    std::size_t definition;
    std::size_t resources;
    std::vector<std::size_t> storages;
    std::vector<std::size_t> inputs;
    std::vector<std::size_t> outputs;

    std::vector<std::size_t> own_values;
    std::vector<std::size_t> own_resources;
    std::vector<std::variant<OperationStep, CallStep>> body;

    std::vector<ShapeContract> input_shapes;
    ValuePorts ports;
    std::vector<std::string> required_inputs;
};

// Shared work is parsed once; each source role borrows the catalog for its entire lifetime.
class TemplateCatalog {
public:
    explicit TemplateCatalog(nlohmann::json document);

    const TemplateDefinition & definition(std::size_t identity) const;
    const TemplateLayout & layout(std::size_t identity) const;
    const ExecutionPattern & execution_pattern(std::size_t identity) const;
    std::size_t layout_count() const;

private:
    void validate() const;

    std::vector<TemplateDefinition> definitions_;
    std::vector<TemplateLayout> layouts_;
    std::vector<ExecutionPattern> execution_patterns_;
};

std::vector<std::int64_t> evaluate_shape(const std::vector<Expression> & shape, const Parameters & parameters);

} // namespace markov
