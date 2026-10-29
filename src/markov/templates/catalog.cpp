#include "markov/templates/catalog.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace markov {

std::vector<std::int64_t> evaluate_shape(const std::vector<Expression> & shape, const Parameters & parameters) {
    std::vector<std::int64_t> dimensions;
    dimensions.reserve(shape.size());
    for (const auto & expression : shape) {
        const auto value = expression.evaluate(parameters);
        if (value < 0) throw std::invalid_argument("negative instantiated tensor dimension");
        dimensions.push_back(value);
    }

    return dimensions;
}

ArgumentTree::ArgumentTree(nlohmann::json source) : source_(std::move(source)) {
    compile(source_, nlohmann::json::json_pointer{});

    // Keep the static tree once; dynamic slots are filled from compiled references on each call.
    for (const auto & [path, _] : references_) source_[path] = nullptr;
    for (const auto & [path, _] : literal_references_) source_[path] = nullptr;
    for (const auto & [path, _] : shapes_) source_[path] = nullptr;
    for (const auto & [path, _] : scalars_) source_[path] = nullptr;
    for (const auto & [path, _] : groups_) source_[path] = nullptr;
}

void ArgumentTree::compile(const nlohmann::json & source, const nlohmann::json::json_pointer & path) {
    if (source.is_object() && source.size() == 1) {
        if (source.contains("literal")) {
            literal_references_.emplace_back(path, source.at("literal").get<std::size_t>());
            return;
        }

        if (source.contains("scalar")) {
            scalars_.emplace_back(path, Expression::read(source.at("scalar")));
            return;
        }

        if (source.contains("input_group")) {
            groups_.emplace_back(path, source.at("input_group").get<std::string>());
            return;
        }

        if (source.contains("value")) {
            references_.emplace_back(path, source.at("value").get<std::size_t>());
            return;
        }

        if (source.contains("shape")) {
            std::vector<Expression> expressions;
            for (const auto & dimension : source.at("shape")) expressions.push_back(Expression::read(dimension));
            shapes_.emplace_back(path, std::move(expressions));
            return;
        }
    }

    if (source.is_array()) {
        for (std::size_t index = 0; index < source.size(); ++index) compile(source[index], path / std::to_string(index));
    }
    else if (source.is_object()) {
        for (const auto & [key, value] : source.items()) compile(value, path / key);
    }
}

nlohmann::json ArgumentTree::instantiate(std::span<const std::size_t> values, const Parameters & parameters, const GroupValues & groups,
                                         const std::vector<nlohmann::json> & literals) const {
    auto result = source_;
    for (const auto & [path, identity] : references_) {
        if (identity >= values.size()) throw std::invalid_argument("argument references an undefined template value");
        result[path] = {
            { "value", values[identity] }
        };
    }

    for (const auto & [path, identity] : literal_references_) result[path] = literals.at(identity);
    for (const auto & [path, shape] : shapes_) result[path] = evaluate_shape(shape, parameters);
    for (const auto & [path, scalar] : scalars_) result[path] = scalar.evaluate(parameters);

    for (const auto & [path, name] : groups_) {
        auto members = nlohmann::json::array();
        for (const auto identity : groups.at(name))
            members.push_back({
                { "value", identity }
            });
        result[path] = std::move(members);
    }

    return result;
}

bool TemplateDefinition::host_only() const {
    return operations.empty() && values.empty() && resources == 0 && state_effects.empty() && input_groups.empty() && guards.empty() && bindings.empty()
           && relations.expressions().empty();
}

TemplateCatalog::TemplateCatalog(nlohmann::json document) {
    for (const auto & source : document.at("execution_patterns")) execution_patterns_.push_back(ExecutionPattern::read(source));

    // Definitions describe local work independently of the caller's layout and concrete bindings.
    for (auto & source : document.at("definitions")) {
        TemplateDefinition definition;
        definition.role = source.at("role").get<std::string>();
        definition.resources = source.at("resources").get<std::size_t>();
        definition.dynamic_calls = source.value("dynamic_calls", false);
        if (source.contains("call_role")) definition.call_role = source.at("call_role").get<std::string>();
        if (source.contains("call_count")) definition.call_count = Expression::read(source.at("call_count"));
        if (source.contains("type")) definition.type = source.at("type").get<std::string>();

        for (const auto & record : source.at("values")) {
            const auto kind = record.at("kind").get<std::string>();
            if (kind != "tensor" && kind != "resource") throw std::invalid_argument("unknown template value kind");

            TemplateValue value{ record.at("storage").get<std::size_t>(),
                                 kind == "tensor" ? ValueKind::Tensor : ValueKind::Resource,
                                 record.at("dtype").get<std::string>(),
                                 record.at("device").get<std::string>(),
                                 {} };
            if (value.kind == ValueKind::Tensor) {
                for (const auto & dimension : record.at("shape")) value.shape.push_back(Expression::read(dimension));
            }
            definition.values.push_back(std::move(value));
        }

        for (auto & work : source.at("body")) {
            Operation access{ work.at("name").get<std::string>(),
                              0,
                              work.at("inputs").get<std::vector<std::size_t>>(),
                              work.at("outputs").get<std::vector<std::size_t>>(),
                              work.at("writes").get<std::vector<std::size_t>>() };

            ArgumentTree arguments(std::move(work.at("arguments")));
            ArgumentTree keywords(std::move(work.at("keywords")));
            std::optional<ArgumentTree> communication;
            if (work.contains("communication")) {
                communication.emplace(std::move(work.at("communication")));
                work.erase("communication");
            }
            const auto groups = work.value("input_groups", std::vector<std::string>{});
            for (const auto & key : { "name", "inputs", "outputs", "writes", "arguments", "keywords", "input_groups" }) work.erase(key);
            definition.operations.push_back(
                { std::move(access), std::move(arguments), std::move(keywords), std::move(work), groups, std::move(communication) });
        }

        if (source.contains("input_groups")) {
            for (const auto & [name, specification] : source.at("input_groups").items()) definition.input_groups.emplace(name, InputGroup::read(specification));
        }

        if (source.contains("parameters")) definition.parameters = source.at("parameters").get<std::vector<std::string>>();
        if (source.contains("bindings")) definition.bindings = source.at("bindings").get<std::vector<std::string>>();
        if (source.contains("guards")) {
            for (const auto & guard : source.at("guards"))
                definition.guards.push_back({ Expression::read(guard.at("condition")), guard.at("expected").get<bool>() });
        }

        if (source.contains("derived_parameters")) definition.relations = ParameterRelations::read(source.at("derived_parameters"));

        // Guards use base inputs; rejected layouts must not demand another branch's derived values.
        std::map<std::string, Expression> derived;
        for (const auto & [name, expression] : definition.relations.expressions()) derived.emplace(name, expression.substitute(derived));
        for (auto & guard : definition.guards) guard.condition = guard.condition.substitute(derived);

        if (source.contains("state_effects")) {
            for (const auto & [resource, effect] : source.at("state_effects").items())
                definition.state_effects.push_back({ resource, Expression::read(effect.at("before")), Expression::read(effect.at("after")) });
        }

        // Replacing a caller's child list must never discard its own operators or state effects.
        if (definition.dynamic_calls && !definition.host_only())
            throw std::invalid_argument("dynamic caller must contain only host service and child calls: " + definition.role);

        definitions_.push_back(std::move(definition));
    }

    // Layouts connect local work to child calls using their own value and resource numbering.
    for (const auto & source : document.at("layouts")) {
        TemplateLayout layout;
        layout.definition = source.at("definition").get<std::size_t>();
        layout.resources = source.at("resources").get<std::size_t>();
        layout.storages = source.at("storages").get<std::vector<std::size_t>>();
        layout.inputs = source.at("inputs").get<std::vector<std::size_t>>();
        layout.outputs = source.at("outputs").get<std::vector<std::size_t>>();
        layout.own_values = source.at("own_values").get<std::vector<std::size_t>>();
        layout.own_resources = source.at("own_resources").get<std::vector<std::size_t>>();
        layout.ports.inputs = source.at("ports").at("inputs").get<std::map<std::string, std::size_t>>();
        layout.ports.outputs = source.at("ports").at("outputs").get<std::map<std::string, std::size_t>>();
        layout.required_inputs = source.value("required_inputs", std::vector<std::string>{});

        for (const auto & contract : source.at("input_shapes")) {
            std::vector<Expression> shape;
            for (const auto & dimension : contract.at("shape")) shape.push_back(Expression::read(dimension));
            layout.input_shapes.push_back({ contract.at("value").get<std::size_t>(),
                                            contract.at("dtype").get<std::string>(),
                                            contract.at("device").get<std::string>(),
                                            std::move(shape) });
        }

        for (const auto & step : source.at("body")) {
            const auto kind = step.at("kind").get<std::string>();
            if (kind == "operation") layout.body.push_back(OperationStep{ step.at("operation").get<std::size_t>() });
            else if (kind == "call") {
                CallStep call{ step.at("layout").get<std::size_t>(),
                               step.at("inputs").get<std::vector<std::size_t>>(),
                               step.at("outputs").get<std::vector<std::size_t>>(),
                               step.at("resources").get<std::vector<std::size_t>>(),
                               step.contains("family") ? std::optional(step.at("family").get<std::string>()) : std::nullopt,
                               {},
                               {} };
                if (step.contains("parameters")) {
                    for (const auto & [name, expression] : step.at("parameters").items()) call.parameters.emplace(name, Expression::read(expression));
                }

                if (step.contains("input_groups")) {
                    for (const auto & [name, binding] : step.at("input_groups").items()) {
                        if (binding.is_string()) call.input_groups.emplace(name, binding.get<std::string>());
                        else if (binding.is_array()) call.input_groups.emplace(name, binding.get<std::vector<std::size_t>>());
                        else {
                            if (binding.contains("value") == binding.contains("producer"))
                                throw std::invalid_argument("generated group requires exactly one element provider");

                            GeneratedGroup group{ Expression::read(binding.at("count")), ReusedGroupValue{ 0 } };
                            if (binding.contains("value")) group.element = ReusedGroupValue{ binding.at("value").get<std::size_t>() };
                            else group.element = GroupProducer{ binding.at("producer").get<std::size_t>() };
                            call.input_groups.emplace(name, std::move(group));
                        }
                    }
                }

                layout.body.push_back(std::move(call));
            }
            else throw std::invalid_argument("unknown template step kind");
        }

        layouts_.push_back(std::move(layout));
    }

    validate();
}

void TemplateCatalog::validate() const {
    for (const auto & definition : definitions_) {
        if ((definition.call_role || definition.call_count) && !definition.dynamic_calls)
            throw std::invalid_argument("target call declarations require a dynamic caller");
        for (const auto & operation : definition.operations) {
            for (const auto & name : operation.input_groups) {
                if (!definition.input_groups.contains(name)) throw std::invalid_argument("operation references an undefined input group: " + name);
            }
        }
    }

    for (std::size_t identity = 0; identity < layouts_.size(); ++identity) {
        const auto & layout = layouts_[identity];
        const auto & definition = definitions_.at(layout.definition);

        for (const auto & [name, value] : layout.ports.inputs) {
            if (std::ranges::find(layout.inputs, value) == layout.inputs.end()) throw std::invalid_argument("named input port is not an input: " + name);
        }
        for (const auto & name : layout.required_inputs) {
            if (!layout.ports.inputs.contains(name)) throw std::invalid_argument("required input has no named port: " + name);
        }
        for (const auto & [name, value] : layout.ports.outputs) {
            if (std::ranges::find(layout.outputs, value) == layout.outputs.end()) throw std::invalid_argument("named output port is not an output: " + name);
        }

        if (layout.own_values.size() != definition.values.size() || layout.own_resources.size() != definition.resources)
            throw std::invalid_argument("own template binding arity mismatch");

        for (const auto & step : layout.body) {
            if (const auto * operation = std::get_if<OperationStep>(&step)) {
                if (operation->operation >= definition.operations.size()) throw std::invalid_argument("template operation is out of range");
                continue;
            }

            const auto & call = std::get<CallStep>(step);
            if (call.layout >= identity) throw std::invalid_argument("template child must precede its parent layout");
            const auto & child = layouts_[call.layout];
            if (call.inputs.size() != child.inputs.size() || call.outputs.size() != child.outputs.size() || call.resources.size() != child.resources)
                throw std::invalid_argument("child template binding arity mismatch");

            const auto & callee = definitions_.at(child.definition);
            for (const auto & [name, _] : call.parameters) {
                if (std::ranges::find(callee.parameters, name) == callee.parameters.end()) throw std::invalid_argument("child has no parameter " + name);
            }

            const auto & groups = callee.input_groups;
            if (call.input_groups.size() != groups.size()) throw std::invalid_argument("child input group bindings are incomplete");
            for (const auto & [name, binding] : call.input_groups) {
                if (!groups.contains(name)) throw std::invalid_argument("child has no input group " + name);
                if (const auto * parent = std::get_if<std::string>(&binding); parent != nullptr && !definition.input_groups.contains(*parent))
                    throw std::invalid_argument("child references an undefined parent input group " + *parent);

                // A repeated producer is safe only when each element is independent of caller state.
                if (const auto * generated = std::get_if<GeneratedGroup>(&binding)) {
                    if (const auto * value = std::get_if<ReusedGroupValue>(&generated->element)) {
                        if (value->value >= layout.storages.size()) throw std::invalid_argument("reused group value is out of range");
                    }
                    else {
                        const auto producer = std::get<GroupProducer>(generated->element).layout;
                        if (producer >= identity) throw std::invalid_argument("group producer must precede its caller layout");

                        const auto & body = layouts_[producer];
                        const auto & producer_definition = definitions_[body.definition];
                        if (!body.inputs.empty() || body.outputs.size() != 1 || !producer_definition.input_groups.empty()
                            || !producer_definition.state_effects.empty() || !producer_definition.parameters.empty()
                            || std::ranges::any_of(body.body, [](const auto & candidate) { return std::holds_alternative<CallStep>(candidate); }))
                            throw std::invalid_argument("group producer must return one fresh value without external inputs or state effects");
                    }
                }
            }
        }
    }
}

const TemplateDefinition & TemplateCatalog::definition(std::size_t identity) const { return definitions_.at(identity); }
const TemplateLayout & TemplateCatalog::layout(std::size_t identity) const { return layouts_.at(identity); }
const ExecutionPattern & TemplateCatalog::execution_pattern(std::size_t identity) const { return execution_patterns_.at(identity); }
std::size_t TemplateCatalog::layout_count() const { return layouts_.size(); }

} // namespace markov
