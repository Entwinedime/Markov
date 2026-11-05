#include "markov/production/source_services.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace markov {

SourceServices::SourceServices(nlohmann::json source) : source_(std::move(source)) {}

nlohmann::json SourceServices::document() {
    auto & regions = source_.at("regions");
    auto & operations = source_.at("operations");
    std::vector<std::optional<std::size_t>> owners(regions.size());
    std::vector<bool> visible(regions.size());
    std::set<std::size_t> exposed;

    for (std::size_t identity = 0; identity < regions.size(); ++identity) {
        const auto & region = regions[identity];
        const auto & metadata = region.at("metadata");
        const auto & parent = region.at("parent");
        if (!parent.is_null()) owners[identity] = owners[parent.get<std::size_t>()];
        const bool dynamic = metadata.value("dynamic_calls", false);
        visible[identity] = dynamic || metadata.contains("entrypoint") || region.at("role") == "module" || parent.is_null();
        if (dynamic) owners[identity] = identity;
        else if (visible[identity]) owners[identity].reset();
        if (!visible[identity]) continue;

        for (const auto * key : { "inputs", "outputs", "mutated_values" }) {
            if (metadata.contains(key)) {
                for (const auto & value : metadata.at(key)) exposed.insert(value.get<std::size_t>());
            }
        }
        if (metadata.contains("source")) exposed.insert(metadata.at("source").get<std::size_t>());
        if (metadata.contains("ports")) {
            for (const auto & ports : metadata.at("ports")) {
                for (const auto & value : ports) exposed.insert(value.get<std::size_t>());
            }
        }
        if (metadata.contains("shape_contracts")) {
            for (const auto & contract : metadata.at("shape_contracts")) exposed.insert(contract.at("value").get<std::size_t>());
        }
        if (metadata.contains("input_groups")) {
            for (const auto & group : metadata.at("input_groups")) {
                for (const auto & member : group.at("members")) exposed.insert(member.at("value").get<std::size_t>());
            }
        }
    }

    std::map<std::size_t, const nlohmann::json *> values;
    for (const auto & value : source_.at("values")) values.emplace(value.at("id").get<std::size_t>(), &value);
    const auto external = source_.at("external_resources").get<std::set<std::size_t>>();
    std::vector<std::optional<std::size_t>> services(operations.size());
    std::set<std::size_t> retained;
    for (const auto & operation : operations) {
        const auto region = operation.at("region").get<std::size_t>();
        const auto owner = owners[region];
        if (!owner) continue;
        services[operation.at("id").get<std::size_t>()] = owner;
        bool closed = operation.contains("execution_observation") && regions[region].at("thread") == regions[*owner].at("thread");
        for (const auto * key : { "inputs", "outputs" }) {
            for (const auto & value : operation.at(key)) closed = closed && values.at(value.get<std::size_t>())->at("device") == "cpu";
        }
        for (const auto & value : operation.at("outputs")) closed = closed && !exposed.contains(value.get<std::size_t>());
        for (const auto & storage : operation.at("writes")) closed = closed && !external.contains(storage.get<std::size_t>());
        if (operation.contains("execution_observation")) {
            const auto & observation = operation.at("execution_observation");
            closed = closed && observation.at("device_work").empty() && observation.at("runtime_actions").empty() && observation.at("communication").is_null()
                     && !observation.contains("native_work");
        }
        if (!closed) retained.insert(*owner);
    }

    // Folding is a whole-caller decision: data or storage crossing its private body keeps that body public.
    for (const auto & edge : source_.at("dependencies")) {
        const auto first = services[edge.at("source").get<std::size_t>()];
        const auto last = services[edge.at("target").get<std::size_t>()];
        if (first == last) continue;
        if (first) retained.insert(*first);
        if (last) retained.insert(*last);
    }
    for (auto & owner : services) {
        if (owner && retained.contains(*owner)) owner.reset();
    }

    // Keep regions used to locate callable descendants; only entirely private helper trees disappear from templates.
    for (auto end = regions.size(); end > 0; --end) {
        const auto identity = end - 1;
        const auto & parent = regions[identity].at("parent");
        if (visible[identity] && !parent.is_null()) visible[parent.get<std::size_t>()] = true;
    }
    for (std::size_t identity = 0; identity < regions.size(); ++identity) {
        if (owners[identity] && !visible[identity] && !retained.contains(*owners[identity])) regions[identity]["host_service_owner"] = *owners[identity];
    }

    std::map<std::size_t, std::size_t> counts;
    std::vector<std::size_t> identities(operations.size());
    auto kept = nlohmann::json::array();
    for (std::size_t identity = 0; identity < operations.size(); ++identity) {
        if (services[identity]) {
            ++counts[*services[identity]];
            continue;
        }
        identities[identity] = kept.size();
        auto record = std::move(operations[identity]);
        record["id"] = identities[identity];
        kept.push_back(std::move(record));
    }
    operations = std::move(kept);

    // Closed private outputs have no public consumer; retain real value IDs while rebasing visible producers.
    auto & value_records = source_["values"].get_ref<nlohmann::json::array_t &>();
    std::erase_if(value_records, [&](const auto & value) {
        const auto & producer = value.at("producer");
        return !producer.is_null() && services[producer.template get<std::size_t>()].has_value();
    });
    for (auto & value : value_records) {
        auto & producer = value.at("producer");
        if (!producer.is_null()) producer = identities[producer.get<std::size_t>()];
    }
    auto & edges = source_["dependencies"].get_ref<nlohmann::json::array_t &>();
    std::erase_if(edges, [&](const auto & edge) {
        return services[edge.at("source").template get<std::size_t>()] || services[edge.at("target").template get<std::size_t>()];
    });
    for (auto & edge : edges) {
        edge["source"] = identities[edge.at("source").get<std::size_t>()];
        edge["target"] = identities[edge.at("target").get<std::size_t>()];
    }
    source_["host_services"] = nlohmann::json::array();
    for (const auto & [region, count] : counts)
        source_["host_services"].push_back({
            { "source_region", region },
            {    "operations",  count }
        });
    return std::move(source_);
}

} // namespace markov
