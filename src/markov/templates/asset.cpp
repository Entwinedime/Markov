#include "markov/templates/asset.hpp"

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <system_error>
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
            const auto groups = work.value("input_groups", std::vector<std::string>{});
            for (const auto & key : { "name", "inputs", "outputs", "writes", "arguments", "keywords", "input_groups" }) work.erase(key);
            definition.operations.push_back({ std::move(access), std::move(arguments), std::move(keywords), std::move(work), groups });
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
                               {} };
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

TemplateAsset::TemplateAsset(const TemplateCatalog & catalog, nlohmann::json document)
    : catalog_(catalog),
      environment_(std::move(document.at("environment"))),
      physical_coverage_(std::move(document.at("physical_coverage"))),
      runtime_coverage_(std::move(document.at("runtime_coverage"))),
      execution_resources_(document.at("execution_resources").get<std::vector<std::string>>()) {
    for (const auto & source : document.at("execution_variants")) {
        ExecutionSelection selection{ source.at("definition").get<std::size_t>(), source.at("operation").get<std::size_t>(), {} };
        for (const auto & choice : source.at("choices")) {
            ExecutionVariant variant{ {}, bound_execution(choice) };
            for (const auto & guard : choice.at("guards"))
                variant.guards.push_back({ Expression::read(guard.at("condition")), guard.at("expected").get<bool>() });
            selection.choices.push_back(std::move(variant));
        }
        if (selection.choices.empty()) throw std::invalid_argument("execution selection has no observed choices");
        execution_variants_.push_back(std::move(selection));
    }

    for (const auto & source : document.at("entries")) entries_.push_back({ source.at("layout").get<std::size_t>(), fixed_bindings(source) });
    for (const auto & source : document.at("entrypoints"))
        entrypoints_.push_back({ source.at("role").get<std::string>(),
                                 source.at("phase").is_null() ? std::nullopt : std::optional(source.at("phase").get<std::string>()),
                                 source.at("entry").get<std::size_t>(),
                                 source.at("path").get<std::vector<std::size_t>>() });

    for (const auto & entry : entries_) validate_bindings(entry.layout, entry.bindings);

    // A shared catalog may include another role's families; only this role's reachable calls need its entrypoints.
    std::vector<CallVisit> visiting(catalog_.layout_count());
    for (const auto & entry : entries_) validate_calls(entry.layout, visiting);
    for (const auto & selection : entrypoints_) validate_calls(frame(selection).layout, visiting);
}

BoundExecution TemplateAsset::bound_execution(const nlohmann::json & source) const {
    auto resources = source.at("resources").get<std::vector<std::size_t>>();
    for (const auto resource : resources) {
        if (resource >= execution_resources_.size()) throw std::invalid_argument("execution binding references an undefined resource");
    }

    const auto pattern = source.at("pattern").get<std::size_t>();
    const auto & execution = catalog_.execution_pattern(pattern);
    if (resources.size() != execution.sequences.size()) throw std::invalid_argument("execution resource binding arity mismatch");
    for (const auto & signal : execution.signals) {
        if (execution_resources_[resources[signal.resource]] != "event") throw std::invalid_argument("event signal requires an event resource");
    }
    for (const auto & required : execution.requirements) {
        if (required.kind == "event_wait" && execution_resources_[resources[required.resource]] != "event")
            throw std::invalid_argument("event wait requires an event resource");
    }
    BoundExecution result{ pattern, std::move(resources), std::nullopt };
    if (source.contains("reduction")) {
        result.reduction = GroupExecution::read(source.at("reduction"));
        result.reduction->validate(execution);
    }
    return result;
}

FixedBindings TemplateAsset::fixed_bindings(const nlohmann::json & source) const {
    FixedBindings result;
    result.literals = source.at("literals").get<std::vector<nlohmann::json>>();
    if (!source.at("host_resource").is_null()) {
        const auto resource = source.at("host_resource").get<std::size_t>();
        if (execution_resources_.at(resource) != "thread") throw std::invalid_argument("region host service requires a thread resource");
        result.host_resource = resource;
    }

    if (source.contains("bindings")) result.values = source.at("bindings").get<Parameters>();
    if (source.contains("execution")) {
        for (const auto & record : source.at("execution")) {
            const auto operation = record.at("operation").get<std::size_t>();
            if (record.contains("variants")) result.execution.emplace(operation, record.at("variants").get<std::size_t>());
            else result.execution.emplace(operation, bound_execution(record));
        }
    }

    for (const auto & [key, value] : source.at("external_resources").items()) {
        std::size_t resource;
        const auto parsed = std::from_chars(key.data(), key.data() + key.size(), resource);
        if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size())
            throw std::invalid_argument("external resource slot must be a nonnegative integer");
        result.external_resources.emplace(resource, value.get<std::size_t>());
    }

    for (const auto & child : source.at("children")) result.children.push_back(fixed_bindings(child));
    if (source.contains("producers")) {
        for (const auto & [name, producer] : source.at("producers").items()) result.producers.emplace(name, fixed_bindings(producer));
    }

    // Observed dynamic parameters and request effects are evidence, never target defaults.
    return result;
}

void TemplateAsset::validate_bindings(std::size_t identity, const FixedBindings & bindings) const {
    const auto & layout = catalog_.layout(identity);
    for (const auto & [operation, binding] : bindings.execution) {
        if (operation >= catalog_.definition(layout.definition).operations.size()) throw std::invalid_argument("execution operation is outside its definition");
        if (const auto * group = std::get_if<std::size_t>(&binding)) {
            const auto & selection = execution_variants_.at(*group);
            if (selection.definition != layout.definition || selection.operation != operation)
                throw std::invalid_argument("execution selection belongs to another logical operation");
        }
    }

    std::size_t child = 0;
    for (const auto & step : layout.body) {
        const auto * call = std::get_if<CallStep>(&step);
        if (!call) continue;
        const auto & frame = bindings.children.at(child++);
        validate_bindings(call->layout, frame);
        for (const auto & [name, group] : call->input_groups) {
            const auto * generated = std::get_if<GeneratedGroup>(&group);
            if (generated) {
                if (const auto * producer = std::get_if<GroupProducer>(&generated->element)) validate_bindings(producer->layout, frame.producers.at(name));
            }
        }
    }
    if (child != bindings.children.size()) throw std::invalid_argument("source child bindings do not match their layout");
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

            const auto & groups = definitions_.at(child.definition).input_groups;
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

void TemplateAsset::validate_calls(std::size_t identity, std::vector<CallVisit> & visiting) const {
    if (visiting.at(identity) == CallVisit::Complete) return;
    if (visiting[identity] == CallVisit::Active) throw std::invalid_argument("template call families contain a cycle");
    visiting[identity] = CallVisit::Active;

    const auto & source = catalog_.layout(identity);
    std::vector<std::string> roles;
    if (const auto & role = catalog_.definition(source.definition).call_role) roles.push_back(*role);
    for (const auto & step : source.body) {
        if (const auto * call = std::get_if<CallStep>(&step)) {
            if (call->family) roles.push_back(*call->family);
            else validate_calls(call->layout, visiting);
        }
    }

    // Family selection may choose a later layout; validate the possible calls rather than source numbering.
    for (const auto & role : roles) {
        bool found = false;
        for (const auto & selection : entrypoints_) {
            if (selection.role != role) continue;
            found = true;
            validate_calls(frame(selection).layout, visiting);
        }
        if (!found) throw std::invalid_argument("template call family has no entrypoint: " + role);
    }
    visiting[identity] = CallVisit::Complete;
}

const TemplateDefinition & TemplateCatalog::definition(std::size_t identity) const { return definitions_.at(identity); }
const TemplateLayout & TemplateCatalog::layout(std::size_t identity) const { return layouts_.at(identity); }
const ExecutionPattern & TemplateCatalog::execution_pattern(std::size_t identity) const { return execution_patterns_.at(identity); }
std::size_t TemplateCatalog::layout_count() const { return layouts_.size(); }

const TemplateDefinition & TemplateAsset::definition(std::size_t identity) const { return catalog_.definition(identity); }
const TemplateLayout & TemplateAsset::layout(std::size_t identity) const { return catalog_.layout(identity); }
const TemplateEntry & TemplateAsset::entry(std::size_t identity) const { return entries_.at(identity); }

TemplateFrame TemplateAsset::frame(const RegionSelection & selection) const {
    const auto & root = entry(selection.entry);
    auto identity = root.layout;
    const auto * bindings = &root.bindings;
    for (const auto child : selection.path) {
        const CallStep * selected = nullptr;
        std::size_t index = 0;
        for (const auto & step : layout(identity).body) {
            if (const auto * call = std::get_if<CallStep>(&step); call != nullptr && index++ == child) {
                selected = call;
                break;
            }
        }
        if (selected == nullptr) throw std::invalid_argument("region selector references an undefined child call");

        identity = selected->layout;
        bindings = &bindings->children.at(child);
    }

    return { identity, *bindings };
}

const nlohmann::json & TemplateAsset::environment() const { return environment_; }
const nlohmann::json & TemplateAsset::physical_coverage() const { return physical_coverage_; }
const nlohmann::json & TemplateAsset::runtime_coverage() const { return runtime_coverage_; }
const std::vector<std::string> & TemplateAsset::execution_resources() const { return execution_resources_; }
const ExecutionPattern & TemplateAsset::execution_pattern(std::size_t identity) const { return catalog_.execution_pattern(identity); }

const BoundExecution & TemplateAsset::execution_binding(const ExecutionBinding & binding, const Parameters & parameters) const {
    if (const auto * fixed = std::get_if<BoundExecution>(&binding)) return *fixed;

    const auto identity = std::get<std::size_t>(binding);
    const BoundExecution * selected = nullptr;
    for (const auto & choice : execution_variants_.at(identity).choices) {
        bool matches = true;
        for (const auto & guard : choice.guards) {
            if ((guard.condition.evaluate(parameters) != 0) != guard.expected) {
                matches = false;
                break;
            }
        }
        if (!matches) continue;
        if (selected) throw std::invalid_argument("multiple execution variants match target for selection " + std::to_string(identity));
        selected = &choice.binding;
    }
    if (!selected) throw LayoutMismatch("no observed execution variant matches target for selection " + std::to_string(identity));
    return *selected;
}

const std::vector<RegionSelection> & TemplateAsset::entrypoints() const { return entrypoints_; }

} // namespace markov
