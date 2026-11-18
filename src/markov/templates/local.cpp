#include "markov/templates/library.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace markov {
namespace {

class LocalCollection {
public:
    explicit LocalCollection(TemplateLibrary & library) : library_(library) {}

    void add(const TemplateFrame & frame, std::vector<std::size_t> & position) {
        const auto contract = library_.local_contract(frame, position);
        if (!contract.is_null() && seen_.emplace(contract, frame.definition, frame.guard.document()).second)
            library_.implementations.push_back({ contract, frame.definition, frame.guard });

        for (std::size_t child = 0; child < frame.children.size(); ++child) {
            position.push_back(child);
            add(frame.children[child], position);
            position.pop_back();
        }
    }

private:
    TemplateLibrary & library_;
    std::set<std::tuple<nlohmann::json, std::size_t, nlohmann::json>> seen_;
};

} // namespace

std::optional<std::size_t> TemplateLibrary::import_leaf(const TemplateDefinition & definition, const TemplateEntry & source,
                                                     std::span<const std::size_t> resource_bindings) {
    if (!definition.children.empty() || !definition.imports.empty() || !definition.kv_accesses.empty() || !definition.slot_accesses.empty()
        || source.frame.communication || !source.frame.bindings.empty())
        return std::nullopt;
    for (const auto & tensor : source.frame.tensors) {
        if (tensor && (!tensor->buffer.empty() || (tensor->external && tensor->writer))) return std::nullopt;
    }
    for (const auto & port : definition.ports) {
        if (port.use == PortUse::Signal || port.rendezvous) return std::nullopt;
    }

    // The adapter binds the declared native operation; the core preserves resource types and original aliasing.
    if (resource_bindings.size() != definition.resources.size()) return std::nullopt;
    auto body = definition;
    auto entry = source;
    const auto original_resources = entry.frame.resources;
    for (std::size_t index = 0; index < resource_bindings.size(); ++index) {
        const auto & resource = resources.at(resource_bindings[index]);
        const auto & slot = definition.resources[index];
        if (resource.kind != slot.kind || resource.serial != slot.serial) return std::nullopt;
        body.resources[index].role = resource.role;
    }
    entry.frame.resources.assign(resource_bindings.begin(), resource_bindings.end());
    for (std::size_t left = 0; left < original_resources.size(); ++left) {
        for (std::size_t right = 0; right < left; ++right) {
            if ((original_resources[left] == original_resources[right]) != (entry.frame.resources[left] == entry.frame.resources[right]))
                return std::nullopt;
        }
    }

    const auto document = body.document();
    const auto found = std::ranges::find_if(definitions, [&](const auto & existing) { return existing.document() == document; });
    entry.frame.definition = static_cast<std::size_t>(found - definitions.begin());
    const auto frame = entry.frame.document();
    for (const auto & existing : entries) {
        if (existing.action == entry.action && existing.condition == entry.condition && existing.frame.document() == frame) return std::nullopt;
    }
    if (found == definitions.end()) definitions.push_back(std::move(body));

    // Imported calls were observed in another library, not at this recipient's scope numbers.
    entry.observations.clear();
    entries.push_back(std::move(entry));
    return entries.size() - 1;
}

nlohmann::json TemplateLibrary::local_contract(const TemplateFrame & frame, const std::vector<std::size_t> & position) const {
    const auto & body = definitions.at(frame.definition);
    if (position.empty() || !frame.bindings.contains("local_implementation") || !body.children.empty() || !body.kv_accesses.empty()
        || !body.slot_accesses.empty())
        return nullptr;
    for (const auto & port : body.ports) {
        if (port.use == PortUse::Signal) return nullptr;
    }

    const auto interface = call_interface(frame);

    // A replacement cannot invent bindings for resources hidden from its caller.
    if (interface.resources.size() != body.resources.size()) return nullptr;

    std::map<std::string, std::size_t> values, storage;
    auto tensors = nlohmann::json::array();
    for (const auto & tensor : frame.tensors) {
        if (!tensor) {
            tensors.push_back(nullptr);
            continue;
        }
        const auto value = values.try_emplace(tensor->value, values.size()).first->second;
        const auto allocation = storage.try_emplace(tensor->storage, storage.size()).first->second;

        // Retain alias/version and local-write semantics, while the caller keeps the actual external writer path.
        tensors.push_back({ { "value", value }, { "storage", allocation }, { "version", tensor->version }, { "external", tensor->external },
                            { "writer", !tensor->writer ? "absent" : *tensor->writer == position ? "local" : "outside" },
                            { "buffer", tensor->buffer } });
    }
    auto communication = nlohmann::json();
    if (frame.communication) {
        const auto & group = *frame.communication;
        const auto member = [&](std::int64_t rank) { return std::ranges::find(group.members, rank) - group.members.begin(); };
        auto required = nlohmann::json::array();
        for (const auto rank : group.required) required.push_back(member(rank));

        // The recipient keeps its rank binding when importing a closed body from another observed participant.
        communication = { { "operation", group.operation }, { "size", group.members.size() }, { "required", std::move(required) } };
        if (frame.bindings.contains("required_peers")) communication.erase("required");
        if (!group.agreements.empty()) communication["agreements"] = group.document().at("agreements");
    }
    return { { "interface", interface.document() },
             { "bindings", frame.bindings }, { "constants", frame.constants }, { "tensors", std::move(tensors) },
             { "communication", std::move(communication) } };
}

void TemplateLibrary::collect_local_implementations() {
    implementations.clear();
    LocalCollection collection(*this);
    std::vector<std::size_t> position;
    for (const auto & entry : entries) collection.add(entry.frame, position);
}

void TemplateLibrary::link_local_implementations(std::span<TemplateLibrary *> libraries) {
    struct Candidate {
        const TemplateLibrary * source;
        std::size_t definition;
        Expression guard;
    };
    using Key = std::pair<nlohmann::json, nlohmann::json>;
    std::map<Key, std::vector<Candidate>> candidates;
    for (const auto * source : libraries) {
        for (const auto & local : source->implementations)
            candidates[{ source->local_domain, local.contract }].push_back({ source, local.definition, local.guard });
    }

    for (auto * target : libraries) {
        auto & library = *target;
        std::set<nlohmann::json> contracts;
        for (const auto & local : library.implementations) contracts.insert(local.contract);
        std::map<nlohmann::json, std::size_t> definitions;
        for (std::size_t index = 0; index < library.definitions.size(); ++index) definitions.emplace(library.definitions[index].document(), index);
        std::set<std::tuple<nlohmann::json, std::size_t, nlohmann::json>> seen;
        library.implementations.clear();

        // Only import bodies for contracts already declared by this recipient; whole actions and domains remain separate.
        for (const auto & contract : contracts) {
            for (const auto & candidate : candidates.at({ library.local_domain, contract })) {

                // Libraries only append definitions; indices survive growth, while guards outlive the cleared candidate lists.
                const auto & body = candidate.source->definitions.at(candidate.definition);
                const auto [found, inserted] = definitions.try_emplace(body.document(), library.definitions.size());
                if (inserted) library.definitions.push_back(body);
                if (seen.emplace(contract, found->second, candidate.guard.document()).second)
                    library.implementations.push_back({ contract, found->second, candidate.guard });
            }
        }
        library.validate();
    }
}

} // namespace markov
