#include "markov/templates/target_graph.hpp"

#include <iterator>
#include <optional>
#include <stdexcept>
#include <utility>

namespace markov {
namespace {

void remap_arguments(nlohmann::json & arguments, const std::vector<std::size_t> & values) {
    if (arguments.is_object() && arguments.size() == 1 && arguments.contains("value")) {
        arguments["value"] = values.at(arguments.at("value").get<std::size_t>());
        return;
    }

    if (arguments.is_structured()) {
        for (auto & item : arguments) remap_arguments(item, values);
    }
}

ValuePorts remap_ports(const ValuePorts & ports, const std::vector<std::size_t> & values) {
    ValuePorts result;
    for (const auto & [name, value] : ports.inputs) result.inputs.emplace(name, values.at(value));
    for (const auto & [name, value] : ports.outputs) result.outputs.emplace(name, values.at(value));
    return result;
}

} // namespace

TargetGraph::TargetGraph(const TemplateAsset & asset, RegionGraph & graph, std::size_t execution_offset)
    : asset_(asset),
      graph_(graph),
      execution_offset_(execution_offset) {}

TargetRegion TargetGraph::append(const std::string & role, const std::string & phase, const Invocation & invocation,
                                 const std::map<std::string, std::size_t> & inputs, const GroupValues & groups) {

    // Candidate selection and port validation finish before the shared graph changes.
    const TargetInputs bindings{ graph_, inputs, groups };
    auto instance = TemplateInstance::select(asset_, role, phase, invocation, &bindings);
    auto & fragment = instance.graph_;
    auto & resource_bindings = instance.bound_inputs_.resources;
    const auto & value_bindings = instance.bound_inputs_.values;
    const auto & bound_types = instance.bound_inputs_.types;

    // External state is shared by source identity; all remaining resources belong to this invocation.
    auto resource_count = graph_.resource_count;
    auto additions = std::map<std::size_t, std::size_t>{};
    for (const auto & [local, source] : fragment.external_resources) {
        if (resource_bindings.at(local)) continue;

        const auto known = external_resources_.find(source);
        if (known != external_resources_.end()) resource_bindings[local] = known->second;
        else {
            const auto [position, added] = additions.try_emplace(source, resource_count);
            if (added) ++resource_count;
            resource_bindings[local] = position->second;
        }
    }

    std::vector<std::size_t> resources;
    for (const auto bound : resource_bindings) resources.push_back(bound ? *bound : resource_count++);

    std::vector<std::size_t> values;
    auto value_count = graph_.values.size();
    for (std::size_t local = 0; local < fragment.values.size(); ++local) {
        const auto bound = value_bindings.find(local);
        values.push_back(bound == value_bindings.end() ? value_count++ : bound->second);
    }

    // Remap the private fragment into the shared identity space before publishing it.
    const auto region_offset = graph_.regions.size();
    const auto operation_offset = graph_.operations.size();
    TargetRegion result{ region_offset, remap_ports(instance.ports_, values), {} };
    for (const auto & [name, members] : instance.group_inputs_) result.group_inputs.emplace(name, remap(members, values));

    for (auto & operation : fragment.operations) {
        operation.region += region_offset;
        operation.inputs = remap(operation.inputs, values);
        operation.outputs = remap(operation.outputs, values);
        operation.writes = remap(operation.writes, resources);
    }

    for (auto & edge : fragment.dependencies) {
        edge.source += operation_offset;
        edge.target += operation_offset;
    }

    for (auto & work : fragment.work) {
        remap_arguments(work["arguments"], values);
        remap_arguments(work["keywords"], values);
    }

    for (auto & region : fragment.regions) {
        region["id"] = region.at("id").get<std::size_t>() + region_offset;
        if (!region.at("parent").is_null()) region["parent"] = region.at("parent").get<std::size_t>() + region_offset;
        for (const auto & key : { "inputs", "outputs" }) region[key] = remap(region.at(key).get<std::vector<std::size_t>>(), values);
        for (auto & ports : region["ports"]) {
            for (auto & value : ports) value = values.at(value.get<std::size_t>());
        }
    }

    for (auto & effect : fragment.state_changes) {
        effect.region += region_offset;
        if (effect.value) effect.value = values.at(*effect.value);
    }

    if (!compositions_.empty()) {
        const auto & caller = compositions_.back();
        fragment.regions.front()["parent"] = caller.identity;
        graph_.execution.append_host(caller.identity, caller.resource);
    }

    for (const auto & [target, type] : bound_types) graph_.types[target] = type;
    for (std::size_t local = 0; local < fragment.values.size(); ++local) {
        if (value_bindings.contains(local)) continue;

        auto value = fragment.values[local];
        value.storage = resources.at(value.storage);
        if (value.producer) *value.producer += operation_offset;
        graph_.values.push_back(value);
        graph_.types.push_back(std::move(fragment.types[local]));
    }

    graph_.operations.insert(graph_.operations.end(), std::make_move_iterator(fragment.operations.begin()), std::make_move_iterator(fragment.operations.end()));
    graph_.dependencies.insert(graph_.dependencies.end(),
                               std::make_move_iterator(fragment.dependencies.begin()),
                               std::make_move_iterator(fragment.dependencies.end()));
    graph_.work.insert(graph_.work.end(), std::make_move_iterator(fragment.work.begin()), std::make_move_iterator(fragment.work.end()));
    graph_.regions.insert(graph_.regions.end(), std::make_move_iterator(fragment.regions.begin()), std::make_move_iterator(fragment.regions.end()));
    graph_.state_changes.insert(graph_.state_changes.end(),
                                std::make_move_iterator(fragment.state_changes.begin()),
                                std::make_move_iterator(fragment.state_changes.end()));
    graph_.execution.append(std::move(fragment.execution), operation_offset, region_offset, execution_offset_);
    graph_.execution.connect_accesses(graph_.values, graph_.types, graph_.operations);

    graph_.resource_count = resource_count;
    external_resources_.merge(additions);
    if (!result.group_inputs.empty()) graph_.regions[result.region]["group_inputs"] = result.group_inputs;

    return result;
}

std::size_t TargetGraph::begin_composition(const std::string & role, const std::string & phase, const Parameters & parameters,
                                           const std::map<std::string, std::size_t> & inputs) {
    for (const auto & selection : asset_.entrypoints()) {
        if (selection.role != role || (selection.phase && *selection.phase != phase)) continue;
        const auto frame = asset_.frame(selection);
        const auto definition_id = asset_.layout(frame.layout).definition;
        const auto & definition = asset_.definition(definition_id);
        if (!definition.host_only()) continue;
        if (!frame.bindings.host_resource) throw LayoutMismatch("dynamic caller has no observed host resource: " + role);
        for (const auto & name : definition.parameters) {
            if (!parameters.contains(name)) throw std::invalid_argument("dynamic caller requires target parameter " + name);
        }

        std::vector<std::size_t> values;
        for (const auto & [_, value] : inputs) {
            graph_.values.at(value);
            values.push_back(value);
        }

        const auto identity = graph_.regions.size();
        nlohmann::json parent = nullptr;
        if (!compositions_.empty()) {
            const auto & caller = compositions_.back();
            parent = caller.identity;
            graph_.execution.append_host(caller.identity, caller.resource);
        }

        // Record the shared definition, not a source layout whose fixed child count was never instantiated.
        graph_.regions.push_back({
            {          "id",                                                          identity },
            {      "parent",                                                            parent },
            {        "role",                                                   definition.role },
            {       "phase",                                                             phase },
            {  "definition",                                                     definition_id },
            { "composition",                                                    "target_calls" },
            {  "parameters",                                                        parameters },
            {      "inputs",                                                            values },
            {     "outputs",                                           nlohmann::json::array() },
            {       "ports", { { "inputs", inputs }, { "outputs", nlohmann::json::object() } } }
        });
        compositions_.push_back({ identity, execution_offset_ + *frame.bindings.host_resource });
        return identity;
    }

    throw LayoutMismatch("no host-only caller source for role " + role + " and phase " + phase);
}

void TargetGraph::end_composition(const std::map<std::string, std::size_t> & outputs) {
    const auto caller = compositions_.back();
    std::vector<std::size_t> values;
    for (const auto & [_, value] : outputs) {
        graph_.values.at(value);
        values.push_back(value);
    }

    auto & region = graph_.regions.at(caller.identity);
    region["outputs"] = std::move(values);
    region["ports"]["outputs"] = outputs;
    graph_.execution.append_host(caller.identity, caller.resource);
    compositions_.pop_back();
}

void TargetGraph::bind_integers(std::size_t value, IntegerData data) {
    auto & type = graph_.types.at(value);
    const nlohmann::json shape = { data.size() };
    if (type.contains("shape") && type.at("shape") != shape) throw LayoutMismatch("integer data does not match its target vector shape");

    const auto existing = graph_.integer_data.find(value);
    if (existing != graph_.integer_data.end()) {
        if (existing->second != data) throw LayoutMismatch("one target value has conflicting integer contents");
        return;
    }

    type["shape"] = shape;
    graph_.integer_data.insert_or_assign(value, std::move(data));
}

const IntegerData & TargetGraph::integers(std::size_t value) const { return graph_.integer_data.at(value); }

void TargetGraph::live_resources(std::set<std::size_t> & resources) const {
    for (const auto & [_, resource] : external_resources_) resources.insert(resource);
}

const RegionGraph & TargetGraph::graph() const { return graph_; }

nlohmann::json TargetGraph::document() const {
    auto result = graph_.document();
    result["scope"] = "composed_logical_regions";
    result["environment"] = asset_.environment();
    result["source_physical_coverage"] = asset_.physical_coverage().at("status");
    result["source_runtime_coverage"] = asset_.runtime_coverage().at("status");
    result["source_runtime_arguments"] = asset_.runtime_coverage().at("native_arguments").at("status");
    result["external_resources"] = external_resources_.size();

    return result;
}

} // namespace markov
