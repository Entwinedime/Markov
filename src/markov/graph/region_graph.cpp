#include "markov/graph/region_graph.hpp"

#include <utility>

namespace markov {

std::vector<std::size_t> RegionGraph::retire(GraphRoots roots) {
    std::map<std::size_t, std::size_t> bindings;
    std::vector<ValueAccess> kept_values;
    std::vector<nlohmann::json> kept_types;
    std::map<std::size_t, IntegerData> kept_integers;
    const auto retain_value = [&](std::size_t value) {
        const auto [binding, added] = bindings.try_emplace(value, bindings.size());
        if (added) {
            const auto storage = values.at(value).storage;
            roots.storage.insert(storage);
            kept_values.push_back({ storage, std::nullopt });
            kept_types.push_back(std::move(types.at(value)));
            if (auto contents = integer_data.extract(value); !contents.empty()) kept_integers.emplace(binding->second, std::move(contents.mapped()));
        }
    };
    for (const auto * root : roots.values) retain_value(*root);

    std::map<std::size_t, std::size_t> region_bindings;
    for (const auto * root : roots.regions) region_bindings.try_emplace(*root, region_bindings.size());

    // Pending messages retain only their operands and enclosing calls, not the completed batch history.
    std::map<std::size_t, std::size_t> operation_bindings;
    for (const auto * root : roots.operations) {
        operation_bindings.try_emplace(*root, operation_bindings.size());
        const auto & operation = operations.at(*root);
        for (const auto value : operation.inputs) retain_value(value);
        for (const auto value : operation.outputs) retain_value(value);
        auto region = operation.region;
        while (true) {
            region_bindings.try_emplace(region, region_bindings.size());
            const auto & parent = regions.at(region).at("parent");
            if (parent.is_null()) break;
            region = parent.get<std::size_t>();
        }
    }
    std::vector<nlohmann::json> kept_regions(region_bindings.size());
    for (const auto & [old_region, new_region] : region_bindings) {
        auto region = std::move(regions.at(old_region));
        region["id"] = new_region;
        if (!region.at("parent").is_null()) region["parent"] = region_bindings.at(region.at("parent").get<std::size_t>());
        for (const auto * key : { "inputs", "outputs" }) {
            for (auto & value : region[key]) {
                const auto identity = value.get<std::size_t>();
                retain_value(identity);
                value = bindings.at(identity);
            }
            for (auto & value : region["ports"][key]) value = bindings.at(value.get<std::size_t>());
        }
        kept_regions[new_region] = std::move(region);
    }

    // Descriptors become incoming state; their CPU readiness remains in the execution frontier.
    auto frontier = execution.retire(values, bindings, roots.storage, region_bindings, operation_bindings);
    std::vector<Operation> kept_operations(operation_bindings.size());
    std::vector<nlohmann::json> kept_work(operation_bindings.size());
    for (const auto & [old_operation, new_operation] : operation_bindings) {
        auto operation = std::move(operations.at(old_operation));
        operation.region = region_bindings.at(operation.region);
        for (auto & value : operation.inputs) value = bindings.at(value);
        for (auto & value : operation.outputs) value = bindings.at(value);
        kept_operations[new_operation] = std::move(operation);
        kept_work[new_operation] = std::move(work.at(old_operation));
        remap_arguments(kept_work[new_operation]["arguments"], bindings);
        remap_arguments(kept_work[new_operation]["keywords"], bindings);
    }
    for (const auto & [old_value, new_value] : bindings) {
        const auto producer = values[old_value].producer;
        if (producer && operation_bindings.contains(*producer)) kept_values[new_value].producer = operation_bindings.at(*producer);
    }
    for (auto * root : roots.values) *root = bindings.at(*root);
    for (auto * root : roots.regions) *root = region_bindings.at(*root);
    for (auto * root : roots.operations) *root = operation_bindings.at(*root);
    values = std::move(kept_values);
    types = std::move(kept_types);
    integer_data = std::move(kept_integers);
    operations = std::move(kept_operations);
    dependencies.clear();
    work = std::move(kept_work);
    regions = std::move(kept_regions);
    state_changes.clear();
    return frontier;
}

nlohmann::json RegionGraph::document() const {
    nlohmann::json result{
        {    "resources",          resource_count },
        {      "regions",                 regions },
        {       "values", nlohmann::json::array() },
        {   "operations", nlohmann::json::array() },
        { "dependencies", nlohmann::json::array() }
    };

    result["execution"] = execution.document();
    if (!execution_resources.empty()) result["execution"]["resources"] = execution_resources;

    // State effects are published by their owning region, independently of tensor readiness.
    for (const auto & effect : state_changes) {
        result["regions"][effect.region]["state_effects"][effect.resource] = {
            { "before", effect.before },
            {  "after",  effect.after }
        };
        if (effect.value) result["regions"][effect.region]["state_effects"][effect.resource]["value"] = *effect.value;
    }

    for (std::size_t identity = 0; identity < values.size(); ++identity) {
        auto value = types[identity];
        if (const auto content = integer_data.find(identity); content != integer_data.end()) value["integer_data"] = content->second.document();

        value.update({
            {       "id",                                                                                         identity },
            {  "storage",                                                                         values[identity].storage },
            { "producer", values[identity].producer ? nlohmann::json(*values[identity].producer) : nlohmann::json(nullptr) }
        });
        result["values"].push_back(std::move(value));
    }

    for (std::size_t identity = 0; identity < operations.size(); ++identity) {
        auto operation = work[identity];
        const auto & access = operations[identity];

        operation.update({
            {      "id",       identity },
            {    "name",    access.name },
            {  "region",  access.region },
            {  "inputs",  access.inputs },
            { "outputs", access.outputs },
            {  "writes",  access.writes }
        });
        result["operations"].push_back(std::move(operation));
    }

    // Derive exported dependencies from the same values and operations that were serialized above.
    auto edges = connect_accesses(values, operations);
    edges.insert(edges.end(), dependencies.begin(), dependencies.end());
    for (const auto & edge : edges) {
        result["dependencies"].push_back({
            { "source", edge.source },
            { "target", edge.target },
            {   "kind",   edge.kind }
        });
    }

    return result;
}

} // namespace markov
