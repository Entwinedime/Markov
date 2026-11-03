#include "markov/production/source_partitions.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace markov {
namespace {

class SourcePartitions {
public:
    explicit SourcePartitions(nlohmann::json source) : source_(std::move(source)) {
        for (const auto & value : source_.at("values")) values_.emplace(value.at("id").get<std::size_t>(), &value);
    }

    nlohmann::json document() {
        const auto count = source_.at("regions").size();
        for (std::size_t identity = 0; identity < count; ++identity) {
            if (source_.at("regions")[identity].at("metadata").contains("resource_calls")) split(identity);
        }
        if (source_.at("regions").size() == count) return std::move(source_);

        // Renumber only semantic ownership; observed host identities remain immutable provenance.
        auto & regions = source_["regions"];
        std::vector<std::vector<std::size_t>> children(regions.size());
        std::vector<std::size_t> roots;
        for (std::size_t identity = 0; identity < regions.size(); ++identity) {
            if (removed_.contains(identity)) continue;
            const auto & parent = regions[identity].at("parent");
            if (parent.is_null()) roots.push_back(identity);
            else children[parent.get<std::size_t>()].push_back(identity);
        }
        std::vector<std::size_t> order;
        for (const auto root : roots) visit(root, children, order);
        std::map<std::size_t, std::size_t> identities;
        for (std::size_t index = 0; index < order.size(); ++index) identities.emplace(order[index], index);

        auto result = nlohmann::json::array();
        for (const auto identity : order) {
            auto record = std::move(regions[identity]);
            record["id"] = identities.at(identity);
            if (!record.at("parent").is_null()) record["parent"] = identities.at(record.at("parent").get<std::size_t>());
            if (!record.at("interface_owner").is_null()) record["interface_owner"] = identities.at(record.at("interface_owner").get<std::size_t>());
            result.push_back(std::move(record));
        }
        regions = std::move(result);
        for (auto & operation : source_["operations"]) operation["region"] = identities.at(operation.at("region").get<std::size_t>());
        return std::move(source_);
    }

private:
    bool contains(std::size_t root, std::size_t child) const {
        const auto & regions = source_.at("regions");
        while (child != root) {
            const auto & parent = regions[child].at("parent");
            if (parent.is_null()) return false;
            child = parent.get<std::size_t>();
        }
        return true;
    }

    static void visit(std::size_t identity, const std::vector<std::vector<std::size_t>> & children, std::vector<std::size_t> & order) {
        order.push_back(identity);
        for (const auto child : children[identity]) visit(child, children, order);
    }

    void split(std::size_t identity) {
        auto & regions = source_["regions"];
        const auto original = regions[identity];
        const auto & declaration = original.at("metadata");
        std::vector<std::size_t> calls;
        for (std::size_t child = 0; child < regions.size(); ++child) {
            const auto & metadata = regions[child].at("metadata");
            if (child != identity && contains(identity, child) && metadata.contains("entrypoint")
                && std::ranges::find(declaration.at("resource_calls"), metadata.at("entrypoint")) != declaration.at("resource_calls").end())
                calls.push_back(child);
        }
        if (calls.empty()) throw std::invalid_argument("partitioned caller has no observed resource call");
        std::ranges::sort(calls,
                          [&](auto left, auto right) { return regions[left].at("host_scope").at("order") < regions[right].at("host_scope").at("order"); });
        const auto first = regions[calls.front()].at("host_scope").at("order").get<std::size_t>();
        const auto last = regions[calls.back()].at("host_scope").at("completion_order").get<std::size_t>();

        std::set<std::size_t> wrappers;
        for (const auto call : calls) {
            auto parent = regions[call].at("parent").get<std::size_t>();
            while (parent != identity) {
                if (regions[parent].at("metadata").contains("entrypoint")) throw std::invalid_argument("resource boundary crosses another public caller");
                wrappers.insert(parent);
                parent = regions[parent].at("parent").get<std::size_t>();
            }
        }
        std::vector<int> parts(regions.size(), -1);
        for (std::size_t child = 0; child < regions.size(); ++child) {
            if (child == identity || !contains(identity, child) || wrappers.contains(child)) continue;
            const auto & owner = regions[child].at("interface_owner");
            if (!owner.is_null() && owner.get<std::size_t>() != child) continue;
            bool resource = false;
            for (const auto call : calls) resource = resource || contains(call, child);
            if (resource) continue;
            const auto & scope = regions[child].at("host_scope");
            if (scope.at("completion_order").get<std::size_t>() <= first) parts[child] = 0;
            else if (scope.at("order").get<std::size_t>() >= last) parts[child] = 1;
            else throw std::invalid_argument("work between resource calls needs its own declared continuation");
        }

        // Body operations can belong directly to a caller whose private wrappers are being folded.
        std::array<std::vector<std::size_t>, 2> direct;
        for (auto & operation : source_["operations"]) {
            const auto owner = operation.at("region").get<std::size_t>();
            if (owner != identity && !wrappers.contains(owner)) continue;
            const auto & scope = operation.at("execution_observation").at("host_scope");
            const auto part = scope.at("completion_order").get<std::size_t>() <= first ? 0 : 1;
            if (part == 1 && scope.at("order").get<std::size_t>() < last) throw std::invalid_argument("caller work crosses a resource boundary");
            direct[part].push_back(operation.at("id").get<std::size_t>());
        }

        const auto prefix = regions.size();
        auto suffix_position = regions[calls.back()].at("operation_begin").get<std::size_t>();
        for (const auto & operation : source_.at("operations")) {
            if (contains(calls.back(), operation.at("region").get<std::size_t>()))
                suffix_position = std::max(suffix_position, operation.at("position").get<std::size_t>() + 1);
        }
        for (int part = 0; part < 2; ++part) {
            auto stage = original;
            stage["id"] = prefix + part;
            stage["parent"] = identity;
            stage["interface_owner"] = nullptr;
            if (part == 1) stage["operation_begin"] = suffix_position;
            stage["host_scope"]["order"] = part == 0 ? original.at("host_scope").at("order") : nlohmann::json(last);
            stage["host_scope"]["completion_order"] = part == 0 ? nlohmann::json(first) : original.at("host_scope").at("completion_order");

            // A structural partition is not an observed call; its source reference retains the actual control seams.
            for (const auto * key : { "started_ns", "ended_ns", "completed" }) stage.erase(key);
            auto & metadata = stage["metadata"];
            const auto role = declaration.at("entrypoint").get<std::string>() + (part == 0 ? "_inputs" : "_outputs");
            metadata["entrypoint"] = role;
            metadata["path"] = role;
            metadata["ports"] = {
                {  "inputs", nlohmann::json::object() },
                { "outputs", nlohmann::json::object() }
            };

            // A continuation still belongs to the original caller's data contract.
            metadata["caller_inputs"] = declaration.at("required_inputs");
            for (const auto * key :
                 { "resource_calls", "inputs", "outputs", "mutated_values", "required_inputs", "opaque_inputs", "opaque_outputs", "shape_contracts" })
                metadata.erase(key);
            regions.push_back(std::move(stage));
        }
        for (std::size_t child = 0; child < parts.size(); ++child) {
            if (parts[child] < 0) continue;
            const auto parent = regions[child].at("parent").get<std::size_t>();
            if (parent == identity || wrappers.contains(parent)) regions[child]["parent"] = prefix + parts[child];
        }
        for (const auto call : calls) regions[call]["parent"] = identity;
        for (int part = 0; part < 2; ++part) {
            for (const auto operation : direct[part]) source_["operations"][operation]["region"] = prefix + part;
        }

        // Public names win; unnamed crossing values use encounter-ordered local names, never source IDs.
        std::map<std::size_t, std::string> names;
        for (const auto & direction : { "inputs", "outputs" }) {
            for (const auto & [name, value] : declaration.at("ports").at(direction).items()) names.try_emplace(value.get<std::size_t>(), name);
        }
        for (const auto call : calls) {
            const auto & metadata = regions[call].at("metadata");
            if (!metadata.contains("ports")) continue;
            for (const auto & direction : { "inputs", "outputs" }) {
                for (const auto & [name, value] : metadata.at("ports").at(direction).items()) names.try_emplace(value.get<std::size_t>(), name);
            }
        }
        for (const auto & child : regions) {
            if (!contains(identity, child.at("id").get<std::size_t>()) || !child.at("metadata").contains("ports")) continue;
            for (const auto & ports : child.at("metadata").at("ports")) {
                for (const auto & [name, value] : ports.items()) names.try_emplace(value.get<std::size_t>(), name);
            }
        }
        std::size_t unnamed = 0;
        std::set<std::size_t> opaque;
        for (const auto & child : regions) {
            if (!contains(identity, child.at("id").get<std::size_t>())) continue;
            for (const auto * key : { "opaque_inputs", "opaque_outputs" }) {
                if (child.at("metadata").contains(key)) {
                    for (const auto & value : child.at("metadata").at(key)) opaque.insert(value.get<std::size_t>());
                }
            }
        }
        for (int part = 0; part < 2; ++part) {
            auto & stage = regions[prefix + part];
            std::set<std::size_t> inputs, outputs, grouped;
            if (declaration.contains("input_groups")) {
                for (const auto & group : declaration.at("input_groups")) {
                    for (const auto & member : group.at("members")) grouped.insert(member.at("value").get<std::size_t>());
                }
            }
            const auto use = [&](std::size_t value, bool inside) {
                const auto & producer = values_.at(value)->at("producer");
                const bool created =
                    !producer.is_null() && contains(prefix + part, source_.at("operations").at(producer.get<std::size_t>()).at("region").get<std::size_t>());
                if (inside && !created && !grouped.contains(value)) inputs.insert(value);
                if (!inside && created) outputs.insert(value);
            };
            for (const auto & operation : source_.at("operations")) {
                const auto inside = contains(prefix + part, operation.at("region").get<std::size_t>());
                for (const auto & value : operation.at("inputs")) use(value.get<std::size_t>(), inside);
            }
            for (const auto & [_, value] : declaration.at("ports").at("outputs").items()) use(value.get<std::size_t>(), false);
            for (const auto & child : regions) {
                if (!contains(identity, child.at("id").get<std::size_t>()) || !child.at("metadata").contains("ports")) continue;
                const bool inside = contains(prefix + part, child.at("id").get<std::size_t>());
                for (const auto & value : child.at("metadata").at("ports").at("inputs")) use(value.get<std::size_t>(), inside);
            }
            auto & metadata = stage["metadata"];
            if (part == 1) {
                outputs.clear();
                for (const auto & value : declaration.at("ports").at("outputs")) {
                    const auto port = value.get<std::size_t>();
                    use(port, true);
                    outputs.insert(port);
                }
            }
            for (const auto & [direction, values] : std::map<std::string, std::set<std::size_t>>{
                     {  "inputs",  inputs },
                     { "outputs", outputs }
            }) {
                std::set<std::string> used;
                for (const auto value : values) {
                    if (!names.contains(value)) names[value] = "live_" + std::to_string(unnamed++);
                    auto name = names.at(value);
                    if (!used.insert(name).second) name += "_" + std::to_string(unnamed++);
                    metadata["ports"][direction][name] = value;
                }
            }
            if (part == 1) metadata["ports"]["outputs"] = declaration.at("ports").at("outputs");
            for (const auto & [key, values] : std::map<std::string, std::set<std::size_t>>{
                     {  "opaque_inputs",  inputs },
                     { "opaque_outputs", outputs }
            }) {
                for (const auto value : values) {
                    if (opaque.contains(value)) metadata[key].push_back(value);
                }
            }
            metadata["required_inputs"] = nlohmann::json::array();
            const auto required = declaration.value("required_inputs", std::set<std::string>{});
            for (const auto & [name, value] : metadata.at("ports").at("inputs").items()) {
                if (!declaration.at("ports").at("inputs").contains(name) || declaration.at("ports").at("inputs").at(name) != value || required.contains(name))
                    metadata["required_inputs"].push_back(name);
            }
            if (declaration.contains("shape_contracts")) {
                for (const auto & contract : declaration.at("shape_contracts")) {
                    if (inputs.contains(contract.at("value").get<std::size_t>())) metadata["shape_contracts"].push_back(contract);
                }
            }
        }

        auto & caller = regions[identity]["metadata"];
        caller = {
            {             "path",             declaration.at("path") },
            {       "entrypoint",       declaration.at("entrypoint") },
            { "entrypoint_phase", declaration.at("entrypoint_phase") },
            {    "dynamic_calls",                               true }
        };
        removed_.insert(wrappers.begin(), wrappers.end());
    }

    nlohmann::json source_;
    std::set<std::size_t> removed_;
    std::map<std::size_t, const nlohmann::json *> values_;
};

} // namespace

nlohmann::json partition_source(nlohmann::json source) { return SourcePartitions(std::move(source)).document(); }

} // namespace markov
