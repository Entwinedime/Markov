#include "markov/graph/region_graph.hpp"

#include <utility>

namespace markov {

std::vector<std::size_t> RegionGraph::retire(std::span<std::size_t *> roots, std::set<std::size_t> retained_storage) {
    std::map<std::size_t, std::size_t> bindings;
    std::vector<ValueAccess> kept_values;
    std::vector<nlohmann::json> kept_types;
    std::map<std::size_t, IntegerData> kept_integers;
    for (auto * root : roots) {
        const auto [binding, added] = bindings.try_emplace(*root, bindings.size());
        if (added) {
            const auto storage = values.at(*root).storage;
            retained_storage.insert(storage);
            kept_values.push_back({ storage, std::nullopt });
            kept_types.push_back(std::move(types.at(*root)));
            if (auto contents = integer_data.extract(*root); !contents.empty()) kept_integers.emplace(binding->second, std::move(contents.mapped()));
        }
    }

    // Descriptors become incoming state; their CPU readiness remains in the execution frontier.
    auto frontier = execution.retire(values, bindings, retained_storage);
    for (auto * root : roots) *root = bindings.at(*root);
    values = std::move(kept_values);
    types = std::move(kept_types);
    integer_data = std::move(kept_integers);
    operations.clear();
    dependencies.clear();
    work.clear();
    regions.clear();
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
