#include "markov/templates/instance.hpp"

#include <algorithm>
#include <charconv>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace markov {
namespace {

void bind_type(nlohmann::json & type, const nlohmann::json & required, const std::string & context) {
    for (const auto & key : { "dtype", "device", "shape" }) {
        if (type.contains(key) && required.contains(key) && type.at(key) != required.at(key))
            throw LayoutMismatch(context + " has conflicting " + key + ": " + type.at(key).dump() + " versus " + required.at(key).dump());
    }

    // An opaque declaration constrains a tensor without erasing its known geometry or identity.
    const bool tensor = type.at("kind") == "tensor";
    type.update(required);
    if (tensor) type["kind"] = "tensor";
}

Parameters read_parameters(const nlohmann::json & source) {
    if (!source.is_object()) throw std::invalid_argument("parameters must be an object");

    Parameters result;
    for (const auto & [name, value] : source.items()) {
        if (!value.is_number_integer()) throw std::invalid_argument("parameter must be an integer: " + name);
        result.emplace(name, value.get<std::int64_t>());
    }

    return result;
}

Parameters bind_parameters(Parameters inherited, const Parameters & fixed, const Invocation & invocation, const ScopedParameters & scopes) {
    for (const auto & [name, value] : fixed) inherited.insert_or_assign(name, value);

    for (const auto & [name, choices] : scopes) {
        const auto original = fixed.find(name);
        const auto target = invocation.bindings.find(name);
        if (original == fixed.end() && target == invocation.bindings.end()) continue;
        const auto identity = target == invocation.bindings.end() ? original->second : target->second;
        for (const auto & [parameter, value] : choices.at(identity)) inherited.insert_or_assign(parameter, value);
    }
    for (const auto * target : { &invocation.parameters, &invocation.bindings }) {
        for (const auto & [name, value] : *target) inherited.insert_or_assign(name, value);
    }
    return inherited;
}

bool bind_input_parameters(const TemplateLayout & layout, const TemplateDefinition & definition, const std::map<std::string, std::size_t> & ports,
                           const RegionGraph & graph, Parameters & arguments) {

    // Direct named axes come from actual ports; compound dimensions remain constraints, never fitted equations.
    for (const auto & contract : layout.input_shapes) {
        const auto named = std::ranges::find_if(layout.ports.inputs, [&](const auto & port) { return port.second == contract.value; });
        if (named == layout.ports.inputs.end()) continue;
        const auto target = ports.find(named->first);
        if (target == ports.end()) continue;

        const auto & type = graph.types.at(target->second);
        if ((type.contains("dtype") && type.at("dtype") != contract.dtype) || (type.contains("device") && type.at("device") != contract.device)) return false;
        if (!type.contains("shape")) continue;
        const auto shape = type.at("shape").get<std::vector<std::int64_t>>();
        if (shape.size() != contract.shape.size()) return false;

        bool known = true;
        for (std::size_t axis = 0; axis < shape.size(); ++axis) {
            const auto expression = contract.shape[axis].document();
            if (expression.is_string()) {
                const auto name = expression.get<std::string>();
                if (!arguments.contains(name) && std::ranges::find(definition.parameters, name) != definition.parameters.end())
                    arguments.emplace(name, shape[axis]);
            }
            for (const auto & name : contract.shape[axis].variables()) known = known && arguments.contains(name);
        }
        if (known && evaluate_shape(contract.shape, arguments) != shape) return false;
    }
    return true;
}

} // namespace

// Borrow only the active call's bindings; recursive expansion cannot outlive these stack values.
struct TemplateInstance::CallContext {
    std::size_t region;
    const FixedBindings & bindings;
    Invocation & invocation;
    const Parameters & parameters;
    const GroupValues & groups;
    const ParameterGroups & sequences;
    const ScopedParameters & scopes;
};

Invocation Invocation::read(const nlohmann::json & source) {
    Invocation result;
    if (source.contains("parameters")) result.parameters = read_parameters(source.at("parameters"));
    if (source.contains("bindings")) result.bindings = read_parameters(source.at("bindings"));

    for (const auto * name : { "input_groups", "sequences" }) {
        if (!source.contains(name)) continue;
        auto & groups = std::string_view(name) == "input_groups" ? result.input_groups : result.sequences;
        for (const auto & [key, members] : source.at(name).items()) {
            if (!members.is_array()) throw std::invalid_argument("parameter group must be an array");
            auto & group = groups[key];
            for (const auto & member : members) group.push_back(read_parameters(member));
        }
    }

    if (source.contains("calls")) {
        for (const auto & [index, child] : source.at("calls").items()) {
            std::size_t identity;
            const auto parsed = std::from_chars(index.data(), index.data() + index.size(), identity);
            if (parsed.ec != std::errc{} || parsed.ptr != index.data() + index.size())
                throw std::invalid_argument("call binding index must be a nonnegative integer");
            result.calls.emplace(identity, read(child));
        }
    }

    return result;
}

TemplateInstance TemplateInstance::select(const TemplateAsset & asset, const std::string & role, const std::string & phase, const Invocation & invocation,
                                          const TargetInputs * inputs) {
    std::set<std::string> mismatches;
    for (const auto & selection : asset.entrypoints()) {
        if (selection.role != role || (selection.phase && *selection.phase != phase)) continue;

        // Follow the recorded call path to preserve the selected region's fixed bindings.
        const auto frame = asset.frame(selection);

        try {
            auto bound = invocation;
            if (inputs != nullptr) {
                const auto & layout = asset.layout(frame.layout);

                // Required values come from their producer; only declared state may remain unbound.
                if (!std::ranges::all_of(layout.required_inputs, [&](const auto & name) { return inputs->ports.contains(name); }))
                    throw LayoutMismatch(role + " requires its declared input ports");
                const auto & definition = asset.definition(layout.definition);
                auto parameters = bind_parameters({}, frame.bindings.values, bound, bound.parameter_scopes);
                if (!bind_input_parameters(layout, definition, inputs->ports, inputs->graph, parameters))
                    throw LayoutMismatch(role + " input geometry differs");
                for (const auto & name : definition.parameters) {
                    if (parameters.contains(name)) bound.parameters.emplace(name, parameters.at(name));
                }
            }
            auto instance = TemplateInstance(asset, frame.layout, frame.bindings, bound, role, phase);
            if (inputs != nullptr) instance.bind_inputs(*inputs);
            return instance;
        }
        catch (const LayoutMismatch & error) {
            mismatches.emplace(error.what());
        }
        catch (const std::invalid_argument & error) {
            throw std::invalid_argument("cannot expand " + role + ": " + error.what());
        }
    }

    std::string message = "no region layout matches " + role + "/" + phase;
    for (const auto & mismatch : mismatches) message += "\n  " + mismatch;
    throw LayoutMismatch(message);
}

void TemplateInstance::bind_inputs(const TargetInputs & inputs) {
    bound_inputs_.resources.resize(graph_.resource_count);
    std::vector<std::pair<std::size_t, std::size_t>> ports;
    for (const auto & [name, target] : inputs.ports) {
        const auto local = ports_.inputs.find(name);
        if (local == ports_.inputs.end()) throw LayoutMismatch(role_ + " has no input port: " + name);
        ports.emplace_back(local->second, target);
    }
    for (const auto & [name, members] : inputs.groups) {
        const auto local = group_inputs_.find(name);
        if (local == group_inputs_.end() || local->second.size() != members.size()) throw LayoutMismatch(role_ + " has a different input group: " + name);
        for (std::size_t index = 0; index < members.size(); ++index) ports.emplace_back(local->second[index], members[index]);
    }

    // Geometry and aliases select an implementation before any target state changes.
    for (const auto & [local, target] : ports) {
        const auto & value = graph_.values.at(local);
        if (value.producer) throw std::invalid_argument("bound input port is produced inside its region");
        const auto [position, added] = bound_inputs_.values.emplace(local, target);
        if (!added && position->second != target) throw LayoutMismatch(role_ + " binds one input to different target values");
        auto & resource = bound_inputs_.resources.at(value.storage);
        const auto storage = inputs.graph.values.at(target).storage;
        if (resource && *resource != storage) throw LayoutMismatch(role_ + " input aliases differ from the target");
        resource = storage;

        auto & type = bound_inputs_.types.try_emplace(target, inputs.graph.types.at(target)).first->second;
        bind_type(type, graph_.types.at(local), role_ + " input " + std::to_string(local));
    }
}

TemplateInstance::TemplateInstance(const TemplateAsset & asset, std::size_t layout, const FixedBindings & bindings, const Invocation & invocation,
                                   std::string role, std::string phase)
    : asset_(asset),
      layout_(layout),
      role_(std::move(role)),
      phase_(std::move(phase)) {
    graph_.resource_count = asset.layout(layout).resources;
    std::vector<std::size_t> resources(graph_.resource_count);
    std::iota(resources.begin(), resources.end(), 0);
    expand(layout,
           bindings,
           invocation,
           {},
           std::nullopt,
           resources,
           std::vector<std::optional<std::size_t>>(asset.layout(layout).storages.size()),
           {},
           invocation.sequences,
           invocation.parameter_scopes);
}

std::size_t TemplateInstance::expand_call(const std::string & role, const Invocation & invocation, const Parameters & parameters, std::size_t parent,
                                          const ValuePorts & ports, const ParameterGroups & sequences, const ScopedParameters & scopes) {
    for (const auto & selection : asset_.entrypoints()) {
        if (selection.role != role || (selection.phase && *selection.phase != phase_)) continue;
        const auto frame = asset_.frame(selection);
        const auto & layout = asset_.layout(frame.layout);
        const auto & definition = asset_.definition(layout.definition);

        auto arguments = bind_parameters(parameters, frame.bindings.values, invocation, scopes);

        // A call family may expose different signatures, such as extend and decode allocation.
        if (layout.ports.inputs.size() != ports.inputs.size()
            || !std::ranges::all_of(ports.inputs, [&](const auto & port) { return layout.ports.inputs.contains(port.first); })
            || !std::ranges::all_of(ports.outputs, [&](const auto & port) { return layout.ports.outputs.contains(port.first); }))
            continue;

        if (!bind_input_parameters(layout, definition, ports.inputs, graph_, arguments)) continue;
        if (!std::ranges::all_of(definition.guards, [&](const auto & guard) { return (guard.condition.evaluate(arguments) != 0) == guard.expected; })) continue;

        // Named boundaries permit a different implementation without inheriting its source's private slots.
        std::vector<std::optional<std::size_t>> slots(layout.storages.size());
        std::vector<std::optional<std::size_t>> bound_resources(layout.resources);
        for (const auto & [local_ports, target_ports] : {
                 std::pair{  &layout.ports.inputs,  &ports.inputs },
                 std::pair{ &layout.ports.outputs, &ports.outputs }
        }) {
            for (const auto & [name, target] : *target_ports) {
                const auto local = local_ports->at(name);
                if (slots[local] && *slots[local] != target) throw LayoutMismatch("call binds one value to different target ports");
                slots[local] = target;

                auto & resource = bound_resources.at(layout.storages[local]);
                const auto storage = graph_.values.at(target).storage;
                if (resource && *resource != storage) throw LayoutMismatch("call port aliases differ from its selected implementation");
                resource = storage;
            }
        }

        for (const auto input : layout.inputs) {
            if (!slots[input])
                throw LayoutMismatch("selected call has an unbound input: " + role + " layout " + std::to_string(frame.layout) + " value "
                                     + std::to_string(input));
        }

        std::vector<std::size_t> resources;
        for (const auto bound : bound_resources) resources.push_back(bound ? *bound : graph_.resource_count++);
        return expand(frame.layout, frame.bindings, invocation, std::move(arguments), parent, resources, std::move(slots), {}, sequences, scopes);
    }

    throw LayoutMismatch("no call implementation matches target: " + role);
}

void TemplateInstance::require_value(std::size_t identity, const nlohmann::json & specification) {
    bind_type(graph_.types.at(identity), specification, "value " + std::to_string(identity));
}

std::size_t TemplateInstance::expand(std::size_t identity, const FixedBindings & bindings, Invocation invocation, Parameters parameters,
                                     std::optional<std::size_t> parent, const std::vector<std::size_t> & resources,
                                     std::vector<std::optional<std::size_t>> slots, GroupValues input_groups, const ParameterGroups & inherited_sequences,
                                     const ScopedParameters & scopes) {
    const auto & layout = asset_.layout(identity);
    const auto & definition = asset_.definition(layout.definition);
    const auto & sequences = invocation.sequences.empty() ? inherited_sequences : invocation.sequences;

    std::vector<bool> bound_resources(layout.resources);
    for (std::size_t slot = 0; slot < slots.size(); ++slot) {
        if (slots[slot]) bound_resources.at(layout.storages[slot]) = true;
    }
    for (const auto & [slot, source] : bindings.external_resources) {

        // An explicit caller port supplies target identity; the callee's captured object is no longer an origin.
        if (bound_resources.at(slot)) continue;
        const auto resource = resources.at(slot);
        const auto [position, added] = graph_.external_resources.emplace(resource, source);
        if (!added && position->second != source) throw std::invalid_argument("conflicting external resource origins");
    }

    // Collection aggregates belong to this call, even when its caller uses the same parameter names.
    for (const auto & [group_name, group] : definition.input_groups) {
        parameters.erase(group.count);
        for (const auto & [name, _] : group.sums) parameters.erase(name);
    }

    // Target arguments override inherited and session bindings; derived values are computed below.
    parameters = bind_parameters(std::move(parameters), bindings.values, invocation, scopes);

    for (const auto & [name, _] : invocation.input_groups) {
        if (!definition.input_groups.contains(name)) throw std::invalid_argument("region has no input group " + name);
    }
    for (const auto & [name, group] : definition.input_groups) {
        if (!invocation.input_groups.contains(name)) {
            if (!input_groups.contains(name)) throw std::invalid_argument("region requires input group " + name);
            const auto & values = input_groups.at(name);
            if (parameters.contains(group.count) && parameters.at(group.count) != static_cast<std::int64_t>(values.size()))
                throw LayoutMismatch("bound input group has a fixed member count: " + name);

            auto & members = invocation.input_groups[name];
            for (const auto value : values) {
                const auto & type = graph_.types.at(value);
                if (!type.contains("shape")) throw LayoutMismatch("bound input group member needs target geometry: " + name);

                const auto geometry = type.at("shape").get<std::vector<std::int64_t>>();
                auto inferred = group.infer_member(geometry, parameters);
                if (!inferred) throw LayoutMismatch("bound input group member does not match its element geometry: " + name);
                members.push_back(std::move(*inferred));
            }
        }

        group.derive(parameters, invocation.input_groups.at(name));
    }

    for (const auto & guard : definition.guards) {
        if ((guard.condition.evaluate(parameters) != 0) != guard.expected)
            throw LayoutMismatch("layout " + std::to_string(identity) + " condition " + guard.condition.document().dump() + " requires "
                                 + (guard.expected ? "true" : "false"));
    }

    for (const auto * required : { &definition.parameters, &definition.bindings }) {
        for (const auto & name : *required) {
            if (!parameters.contains(name)) throw std::invalid_argument("layout " + std::to_string(identity) + " requires target parameter " + name);
        }
    }

    for (const auto & [name, _] : definition.relations.expressions()) {
        if (invocation.parameters.contains(name) || invocation.bindings.contains(name))
            throw std::invalid_argument("target must not supply derived parameter " + name);
    }
    definition.relations.derive(parameters);

    // Allocate only unbound value slots; parent ports retain their storage identity.
    std::vector<std::size_t> values;
    values.reserve(slots.size());
    for (std::size_t index = 0; index < slots.size(); ++index) {
        const auto storage = resources.at(layout.storages[index]);
        if (!slots[index]) {
            slots[index] = graph_.values.size();
            graph_.values.push_back({ storage, std::nullopt });
            graph_.types.push_back({
                { "kind", "resource" }
            });
        }
        else if (graph_.values.at(*slots[index]).storage != storage) throw std::invalid_argument("child value/resource alias binding mismatch");
        values.push_back(*slots[index]);
    }

    if (!parent) {
        inputs_ = remap(layout.inputs, values);
        outputs_ = remap(layout.outputs, values);
    }

    for (const auto & contract : layout.input_shapes) {
        require_value(values.at(contract.value),
                      {
                          { "kind", "tensor" },
                          { "dtype", contract.dtype },
                          { "device", contract.device },
                          { "shape", evaluate_shape(contract.shape, parameters) }
        });
    }

    auto region_inputs = remap(layout.inputs, values);
    for (const auto & [name, group] : definition.input_groups) {
        const auto & members = invocation.input_groups.at(name);
        auto [position, allocated] = input_groups.try_emplace(name);
        auto & identities = position->second;
        if (!allocated && identities.size() != members.size()) throw LayoutMismatch("child input group arity differs from its parent binding");

        for (std::size_t index = 0; index < members.size(); ++index) {
            if (allocated) {
                identities.push_back(graph_.values.size());
                graph_.values.push_back({ graph_.resource_count++, std::nullopt });
                graph_.types.push_back({
                    { "kind", "resource" }
                });
            }

            require_value(identities[index],
                          {
                              { "kind", "tensor" },
                              { "dtype", group.member_dtype(index) },
                              { "device", group.device },
                              { "shape", group.dimensions(parameters, members[index]) }
            });
        }

        region_inputs.insert(region_inputs.end(), identities.begin(), identities.end());
    }

    if (!parent) {
        inputs_ = region_inputs;
        group_inputs_ = input_groups;
    }

    const auto own_values = remap(layout.own_values, values);
    const auto own_resources = remap(layout.own_resources, resources);
    const auto region = graph_.regions.size();
    nlohmann::json metadata{
        { "id", region },
        { "parent", parent ? nlohmann::json(*parent) : nlohmann::json(nullptr) },
        { "role", definition.role },
        { "layout", identity },
        { "inputs", region_inputs },
        { "outputs", remap(layout.outputs, values) },
        { "parameters", parameters }
    };

    ValuePorts named_ports;
    for (const auto & [name, value] : layout.ports.inputs) named_ports.inputs.emplace(name, values.at(value));
    for (const auto & [name, value] : layout.ports.outputs) named_ports.outputs.emplace(name, values.at(value));
    metadata["ports"] = {
        {  "inputs",  named_ports.inputs },
        { "outputs", named_ports.outputs }
    };
    if (!definition.input_groups.empty()) metadata["input_groups"] = invocation.input_groups;
    if (definition.type) metadata["type"] = *definition.type;

    for (const auto & effect : definition.state_effects) {
        std::optional<std::size_t> state_value;
        if (const auto output = named_ports.outputs.find(effect.resource); output != named_ports.outputs.end()) state_value = output->second;
        graph_.state_changes.push_back({ region, effect.resource, effect.before.evaluate(parameters), effect.after.evaluate(parameters), state_value });
    }

    if (!parent) ports_ = named_ports;
    graph_.regions.push_back(std::move(metadata));

    // Only the declared host caller changes its child list; its parent's work remains intact.
    if (definition.dynamic_calls) {
        const auto found = sequences.find(definition.role);
        if (found == sequences.end() || !definition.call_role) throw LayoutMismatch("region requires target child composition: " + definition.role);
        const auto & sequence = found->second;
        if (definition.call_count && definition.call_count->evaluate(parameters) != static_cast<std::int64_t>(sequence.size()))
            throw LayoutMismatch("target call sequence does not match its declared count: " + definition.role);

        auto current = named_ports.inputs;
        for (std::size_t index = 0; index < sequence.size(); ++index) {
            if (bindings.host_resource) graph_.execution.append_host(region, *bindings.host_resource);
            Invocation member;
            member.parameters = sequence[index];
            const bool last = index + 1 == sequence.size();
            const auto child = expand_call(*definition.call_role,
                                           member,
                                           parameters,
                                           region,
                                           { current, last ? named_ports.outputs : std::map<std::string, std::size_t>{} },
                                           sequences,
                                           scopes);

            // Only declared loop inputs carry state between members; other child outputs stay private.
            for (const auto & [name, value] : graph_.regions.at(child).at("ports").at("outputs").items()) {
                if (last || current.contains(name)) current.insert_or_assign(name, value.get<std::size_t>());
            }
        }
        for (const auto & [name, value] : named_ports.outputs) {
            if (current.at(name) != value) throw LayoutMismatch("dynamic call sequence did not produce its declared output: " + name);
        }
        if (bindings.host_resource) graph_.execution.append_host(region, *bindings.host_resource);
        return region;
    }

    const CallContext context{ region, bindings, invocation, parameters, input_groups, sequences, scopes };
    std::size_t child_index = 0;
    for (const auto & step : layout.body) {

        // shortcut: caller overhead for closed producer groups precedes generation; refine when iteration issue cadence matters.
        if (bindings.host_resource) graph_.execution.append_host(region, *bindings.host_resource);
        if (const auto * own = std::get_if<OperationStep>(&step)) expand_operation(own->operation, definition, context, own_values, own_resources);
        else expand_child(std::get<CallStep>(step), child_index++, context, values, resources);
    }

    if (!invocation.calls.empty() && invocation.calls.rbegin()->first >= child_index)
        throw std::invalid_argument("target binding references an undefined child call");
    if (bindings.host_resource) graph_.execution.append_host(region, *bindings.host_resource);

    return region;
}

void TemplateInstance::expand_operation(std::size_t identity, const TemplateDefinition & definition, const CallContext & context,
                                        const std::vector<std::size_t> & own_values, const std::vector<std::size_t> & own_resources) {
    const auto & bindings = context.bindings;
    const auto & invocation = context.invocation;
    const auto & parameters = context.parameters;
    const auto & input_groups = context.groups;
    const auto region = context.region;

    const auto & prototype = definition.operations.at(identity);
    auto operation = prototype.access;
    operation.region = region;
    operation.inputs = remap(operation.inputs, own_values);
    for (const auto & name : prototype.input_groups) {
        const auto & members = input_groups.at(name);
        operation.inputs.insert(operation.inputs.end(), members.begin(), members.end());
    }
    operation.outputs = remap(operation.outputs, own_values);
    operation.writes = remap(operation.writes, own_resources);

    for (const bool output : { false, true }) {
        const auto & sources = output ? prototype.access.outputs : prototype.access.inputs;
        for (const auto source : sources) {
            const auto value = own_values.at(source);
            const auto & spec = definition.values.at(source);
            if (graph_.values[value].storage != own_resources.at(spec.storage))
                throw std::invalid_argument(output ? "output storage binding mismatch" : "input storage binding mismatch");

            // Inputs retain producer geometry; outputs establish the operation's declared shape.
            const bool infer_shape = spec.kind == ValueKind::Tensor && (output || !graph_.types[value].contains("shape"));
            nlohmann::json type{
                {   "kind", infer_shape ? "tensor" : "resource" },
                {  "dtype",                          spec.dtype },
                { "device",                         spec.device }
            };
            if (infer_shape) type["shape"] = evaluate_shape(spec.shape, parameters);
            require_value(value, type);

            if (output && std::ranges::find(operation.inputs, value) == operation.inputs.end()) {
                if (graph_.values[value].producer) throw std::invalid_argument("multiple producers for one target value");
                graph_.values[value].producer = graph_.operations.size();
            }
        }
    }

    auto work = prototype.properties;
    work["arguments"] = prototype.arguments.instantiate(own_values, parameters, input_groups, bindings.literals);
    work["keywords"] = prototype.keywords.instantiate(own_values, parameters, input_groups, bindings.literals);
    if (prototype.communication) work["communication"] = prototype.communication->instantiate(own_values, parameters, input_groups, bindings.literals);
    if (const auto found = bindings.execution.find(identity); found != bindings.execution.end()) {
        const auto & execution = asset_.execution_binding(found->second, parameters);
        const auto & pattern = asset_.execution_pattern(execution.pattern);
        if (execution.reduction) {
            const auto & group = execution.reduction->group();
            auto plan = execution.reduction->instantiate(pattern, definition.input_groups.at(group), parameters, invocation.input_groups.at(group));
            graph_.execution.append(plan.pattern, execution.resources, graph_.operations.size(), region);
            work["execution_parts"] = std::move(plan.parts);
        }
        else graph_.execution.append(pattern, execution.resources, graph_.operations.size(), region);
        work["execution_pattern"] = execution.pattern;
    }

    graph_.operations.push_back(std::move(operation));
    graph_.work.push_back(std::move(work));
}

void TemplateInstance::expand_child(const CallStep & call, std::size_t child_index, const CallContext & context, const std::vector<std::size_t> & values,
                                    const std::vector<std::size_t> & resources) {
    const auto & bindings = context.bindings;
    auto & invocation = context.invocation;
    const auto & parameters = context.parameters;
    const auto & input_groups = context.groups;
    const auto & sequences = context.sequences;
    const auto & scopes = context.scopes;
    const auto region = context.region;

    // Child calls share boundary values while allocating their private slots recursively.
    const auto & child = asset_.layout(call.layout);
    std::vector<std::optional<std::size_t>> child_values(child.storages.size());
    for (const auto * ports : { &call.inputs, &call.outputs }) {
        const auto & target = ports == &call.inputs ? child.inputs : child.outputs;
        for (std::size_t port = 0; port < target.size(); ++port) {
            auto & bound = child_values.at(target[port]);
            const auto value = values.at(ports->at(port));
            if (bound && *bound != value) throw std::invalid_argument("one child port has conflicting value bindings");
            bound = value;
        }
    }

    const auto found = invocation.calls.find(child_index);
    auto child_invocation = found == invocation.calls.end() ? Invocation{} : std::move(found->second);

    // Scalar arguments are evaluated in the caller, then bound to the callee's local names.
    for (const auto & [name, expression] : call.parameters) {
        if (child_invocation.parameters.contains(name) || child_invocation.bindings.contains(name))
            throw std::invalid_argument("bound parameter is supplied by its parent: " + name);
        child_invocation.parameters.emplace(name, expression.evaluate(parameters));
    }

    GroupValues child_groups;
    for (const auto & [name, binding] : call.input_groups) {
        if (child_invocation.input_groups.contains(name)) throw std::invalid_argument("bound input group is supplied by its parent: " + name);
        if (const auto * group = std::get_if<std::string>(&binding)) {
            child_groups.emplace(name, input_groups.at(*group));
            child_invocation.input_groups.emplace(name, invocation.input_groups.at(*group));
        }
        else if (const auto * fixed = std::get_if<std::vector<std::size_t>>(&binding)) child_groups.emplace(name, remap(*fixed, values));
        else {
            const auto & generated = std::get<GeneratedGroup>(binding);
            const auto count = generated.count.evaluate(parameters);
            if (count < 0) throw std::invalid_argument("group generation needs a nonnegative count");

            auto & members = child_groups[name];
            members.reserve(static_cast<std::size_t>(count));
            if (const auto * reused = std::get_if<ReusedGroupValue>(&generated.element))
                members.assign(static_cast<std::size_t>(count), values.at(reused->value));
            else {
                const auto producer = std::get<GroupProducer>(generated.element).layout;
                const auto & prototype = asset_.layout(producer);
                for (std::int64_t index = 0; index < count; ++index) {
                    std::vector<std::size_t> producer_resources(prototype.resources);
                    std::iota(producer_resources.begin(), producer_resources.end(), graph_.resource_count);
                    graph_.resource_count += prototype.resources;

                    const auto produced = expand(producer,
                                                 bindings.children.at(child_index).producers.at(name),
                                                 {},
                                                 parameters,
                                                 region,
                                                 producer_resources,
                                                 std::vector<std::optional<std::size_t>>(prototype.storages.size()),
                                                 {},
                                                 sequences,
                                                 scopes);
                    members.push_back(graph_.regions.at(produced).at("outputs").at(0).get<std::size_t>());
                }
            }
        }
    }

    if (call.family) {

        // A reusable implementation keeps the caller's role identity, such as its layer and expert shard.
        for (const auto & [name, value] : bindings.children.at(child_index).values) child_invocation.bindings.try_emplace(name, value);

        for (const auto & contract : child.input_shapes) {
            if (!child_values.at(contract.value)) continue;
            bool known = true;
            for (const auto & dimension : contract.shape) {
                for (const auto & name : dimension.variables()) known = known && parameters.contains(name);
            }
            if (!known) continue;

            // The caller supplies fixed geometry even when the callee uses its own local axis names.
            require_value(*child_values[contract.value],
                          {
                              { "kind", "tensor" },
                              { "dtype", contract.dtype },
                              { "device", contract.device },
                              { "shape", evaluate_shape(contract.shape, parameters) }
            });
        }
        ValuePorts bound;
        for (const auto & [name, value] : child.ports.inputs) bound.inputs.emplace(name, child_values.at(value).value());
        for (const auto & [name, value] : child.ports.outputs) bound.outputs.emplace(name, child_values.at(value).value());
        expand_call(*call.family, child_invocation, parameters, region, bound, sequences, scopes);
    }
    else
        expand(call.layout,
               bindings.children.at(child_index),
               std::move(child_invocation),
               parameters,
               region,
               remap(call.resources, resources),
               std::move(child_values),
               std::move(child_groups),
               sequences,
               scopes);
}

const std::vector<StateChange> & TemplateInstance::state_changes() const { return graph_.state_changes; }
const ValuePorts & TemplateInstance::ports() const { return ports_; }

nlohmann::json TemplateInstance::document() {
    graph_.execution.connect_accesses(graph_.values, graph_.types, graph_.operations);
    auto result = graph_.document();
    result["execution"]["resources"] = asset_.execution_resources();
    result["group_inputs"] = group_inputs_;
    result.update({
        {              "environment",                                           asset_.environment() },
        {                   "layout",                                                        layout_ },
        {                    "scope",                                      "logical_region_instance" },
        { "source_physical_coverage",                        asset_.physical_coverage().at("status") },
        {  "source_runtime_coverage",                         asset_.runtime_coverage().at("status") },
        { "source_runtime_arguments",  asset_.runtime_coverage().at("native_arguments").at("status") },
        {                     "role",                                                          role_ },
        {                    "phase",                                                         phase_ },
        {                   "inputs",                                                        inputs_ },
        {                  "outputs",                                                       outputs_ },
        {                    "ports", { { "inputs", ports_.inputs }, { "outputs", ports_.outputs } } }
    });
    return result;
}

} // namespace markov
