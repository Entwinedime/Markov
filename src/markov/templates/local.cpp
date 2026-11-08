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

nlohmann::json TemplateLibrary::local_contract(const TemplateFrame & frame, const std::vector<std::size_t> & position) const {
    const auto & body = definitions.at(frame.definition);
    if (position.empty() || !frame.bindings.contains("local_implementation") || !body.children.empty() || !body.kv_accesses.empty()
        || !body.slot_accesses.empty())
        return nullptr;
    for (const auto & port : body.ports) {
        if (port.wait || port.use == PortUse::Signal) return nullptr;
    }

    const auto event = [&](const TemplateReference & reference) { return reference.event + (reference.imported ? 0 : body.imports.size()); };
    const auto resource = [&](const TemplateReference & reference) {
        return reference.imported ? body.imports.at(reference.event) : body.events.at(reference.event);
    };
    std::vector<std::size_t> boundaries, slots;
    for (std::size_t imported = 0; imported < body.imports.size(); ++imported) {
        boundaries.push_back(imported);
        slots.push_back(body.imports[imported]);
    }
    for (const auto & exported : body.exports) {
        boundaries.push_back(event(exported));
        slots.push_back(resource(exported));
    }
    for (const auto & port : body.ports) {
        boundaries.push_back(event(port.event));
        slots.push_back(resource(port.event));
        if (port.rendezvous) {
            boundaries.push_back(event(*port.rendezvous));
            slots.push_back(resource(*port.rendezvous));
        }
    }

    // Every resource slot must have a caller-visible identity; private extra resources cannot be guessed on replacement.
    if (std::set<std::size_t>(slots.begin(), slots.end()).size() != body.resources.size()) return nullptr;
    std::vector<std::vector<std::size_t>> successors(body.imports.size() + body.events.size());
    for (const auto & edge : body.edges) successors[event(edge.source)].push_back(event(edge.target));
    for (const auto & work : body.work) successors[event(work.start)].push_back(event(work.finish));
    auto causality = nlohmann::json::array();
    for (std::size_t origin = 0; origin < boundaries.size(); ++origin) {
        std::vector<bool> reached(successors.size());
        auto pending = successors[boundaries[origin]];
        while (!pending.empty()) {
            const auto current = pending.back();
            pending.pop_back();
            if (reached[current]) continue;
            reached[current] = true;
            pending.insert(pending.end(), successors[current].begin(), successors[current].end());
        }
        for (std::size_t target = 0; target < boundaries.size(); ++target) {
            if (reached[boundaries[target]]) causality.push_back({ origin, target });
        }
    }

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
        communication = { { "operation", group.operation }, { "size", group.members.size() }, { "rank", member(group.rank) },
                          { "required", std::move(required) } };
    }
    return { { "interface", call_interface(frame).document() }, { "slots", slots }, { "causality", std::move(causality) },
             { "bindings", frame.bindings }, { "constants", frame.constants }, { "tensors", std::move(tensors) },
             { "communication", std::move(communication) } };
}

void TemplateLibrary::collect_local_implementations() {
    implementations.clear();
    LocalCollection collection(*this);
    std::vector<std::size_t> position;
    for (const auto & entry : entries) collection.add(entry.frame, position);
}

void TemplateLibrary::link_local_implementations(std::span<TemplateLibrary> libraries) {
    using Key = std::pair<nlohmann::json, nlohmann::json>;
    std::map<Key, std::vector<std::pair<TemplateDefinition, Expression>>> candidates;
    for (const auto & library : libraries) {
        for (const auto & local : library.implementations)
            candidates[{ library.local_domain, local.contract }].emplace_back(library.definitions.at(local.definition), local.guard);
    }

    for (auto & library : libraries) {
        std::set<nlohmann::json> contracts;
        for (const auto & local : library.implementations) contracts.insert(local.contract);
        std::map<nlohmann::json, std::size_t> definitions;
        for (std::size_t index = 0; index < library.definitions.size(); ++index) definitions.emplace(library.definitions[index].document(), index);
        std::set<std::tuple<nlohmann::json, std::size_t, nlohmann::json>> seen;
        library.implementations.clear();

        // Only import bodies for contracts already declared by this recipient; whole actions and domains remain separate.
        for (const auto & contract : contracts) {
            for (const auto & [body, guard] : candidates.at({ library.local_domain, contract })) {
                const auto [found, inserted] = definitions.try_emplace(body.document(), library.definitions.size());
                if (inserted) library.definitions.push_back(body);
                if (seen.emplace(contract, found->second, guard.document()).second)
                    library.implementations.push_back({ contract, found->second, guard });
            }
        }
        library.validate();
    }
}

} // namespace markov
