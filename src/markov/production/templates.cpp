#include "markov/production/templates.hpp"
#include "markov/production/entries.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace markov {
namespace {

struct ScopeCompilation {
    std::vector<std::size_t> children;
    std::vector<std::size_t> events;
    std::vector<std::size_t> work;
    std::vector<std::size_t> edges;
    std::vector<std::size_t> imports;
    std::map<std::size_t, std::size_t> exports;
    TemplateFrame frame;
};

class LibraryCompilation {
public:
    LibraryCompilation(const CoarseGraph & source, TemplateLibrary & library, std::map<nlohmann::json, std::size_t> & shared)
        : source_(source),
          library_(library),
          shared_(shared),
          scopes_(source.scopes.size()),
          child_positions_(source.scopes.size()),
          event_positions_(source.events.size()) {
        std::map<std::string, std::size_t> value_owners, storage_owners;
        const auto observe_owner = [&](auto & owners, const auto & identity, std::size_t scope) {
            const auto [found, inserted] = owners.try_emplace(identity, scope);
            if (!inserted) found->second = source.common_scope(found->second, scope);
        };
        for (std::size_t scope = 0; scope < source.scopes.size(); ++scope) {
            for (const auto & port : source.scopes[scope].ports) {
                if (!port.tensor) continue;
                const auto & tensor = *port.tensor;
                const auto owner = tensor.writer ? source.common_scope(scope, *tensor.writer) : scope;
                observe_owner(value_owners, tensor.value, owner);
                observe_owner(storage_owners, tensor.storage, owner);
                auto [version, inserted] = versions_.try_emplace(tensor.storage, tensor.version);
                if (!inserted) version->second = std::min(version->second, tensor.version);
                if (port.use == PortUse::Data && !tensor.allocation) external_.insert(tensor.storage);
            }
        }
        for (std::size_t scope = 1; scope < source.scopes.size(); ++scope) {
            auto & siblings = scopes_[*source.scopes[scope].info.parent].children;
            child_positions_[scope] = siblings.size();
            siblings.push_back(scope);
        }

        // Private identities belong to their smallest shared scope, so changing one subtree cannot renumber its siblings.
        std::vector<std::string> spaces(source.scopes.size(), "scope/");
        for (std::size_t scope = 1; scope < source.scopes.size(); ++scope)
            spaces[scope] = spaces[*source.scopes[scope].info.parent] + std::to_string(child_positions_[scope]) + "/";
        for (const auto & storage : external_) storage_owners.at(storage) = 0;
        std::vector<std::size_t> value_counts(scopes_.size()), storage_counts(scopes_.size());
        for (const auto & scope : source.scopes) {
            for (const auto & port : scope.ports) {
                if (!port.tensor) continue;
                const auto & tensor = *port.tensor;
                const auto value_owner = value_owners.at(tensor.value), storage_owner = storage_owners.at(tensor.storage);
                if (!values_.contains(tensor.value))
                    values_.emplace(tensor.value, spaces[value_owner] + "value/" + std::to_string(value_counts[value_owner]++));
                if (!storage_.contains(tensor.storage))
                    storage_.emplace(tensor.storage, spaces[storage_owner] + "storage/" + std::to_string(storage_counts[storage_owner]++));
            }
        }
        for (std::size_t event = 0; event < source.events.size(); ++event) {
            auto & local = scopes_[source.events[event].scope].events;
            event_positions_[event] = local.size();
            local.push_back(event);
        }
        for (std::size_t work = 0; work < source.work.size(); ++work) scopes_[source.work[work].scope].work.push_back(work);

        // Derive interfaces before interning definitions; later callers cannot mutate a shared child's body.
        std::vector<std::set<std::size_t>> boundaries(source.events.size());
        const auto boundary = [&](std::size_t scope, std::size_t event) {
            boundaries[event].insert(scope);
            expose(scope, event);
        };
        for (std::size_t scope = 0; scope < source.scopes.size(); ++scope) {
            for (const auto & port : source.scopes[scope].ports) {
                boundary(scope, port.event);
                if (port.wait) boundary(scope, port.wait->begin);
                if (port.rendezvous) boundary(scope, *port.rendezvous);
            }
            for (const auto & access : source.scopes[scope].slot_accesses) {
                boundary(scope, access.start);
                boundary(scope, access.finish);
            }
            for (const auto & access : source.scopes[scope].kv_accesses) {
                boundary(scope, access.start);
                boundary(scope, access.finish);
            }
        }
        for (const auto & piece : source.work) {
            boundary(piece.scope, piece.start);
            boundary(piece.scope, piece.finish);
        }
        for (std::size_t index = 0; index < source.edges.size(); ++index) {
            const auto & edge = source.edges[index];
            const auto owner = edge_owner(edge, boundaries);
            scopes_[owner].edges.push_back(index);
            expose(owner, edge.source);
            expose(owner, edge.target);
        }
        for (auto & scope : scopes_) {

            // Callers may encounter the same endpoints in different orders; the interface follows source boundary order.
            std::size_t position = 0;
            for (auto & [event, index] : scope.exports) index = position++;
        }
    }

    TemplateEntry compile(const EntrySpecification & entry) {

        // Compile children first to derive their call interfaces before sharing the caller's structure.
        for (std::size_t scope = scopes_.size(); scope-- > 0;) compile_scope(scope);
        return { entry.action, entry.condition, std::move(scopes_[0].frame), { source_.scopes[0].info.parameters } };
    }

private:
    std::size_t edge_owner(const GraphEdge & edge, const std::vector<std::set<std::size_t>> & boundaries) const {
        const auto owner = source_.common_scope(source_.events[edge.source].scope, source_.events[edge.target].scope);
        std::optional<std::size_t> candidate;
        for (const auto & [external, internal] : { std::pair{ edge.source, edge.target }, std::pair{ edge.target, edge.source } }) {
            for (const auto scope : boundaries[external]) {
                if (scope == owner || !source_.contains(owner, scope) || !source_.contains(scope, source_.events[internal].scope)) continue;

                // A shared endpoint can belong to a parent while the child's declared operation owns its dependency.
                if (!candidate || source_.contains(*candidate, scope)) candidate = scope;
                else if (!source_.contains(scope, *candidate)) return owner;
            }
        }
        return candidate.value_or(owner);
    }

    void expose(std::size_t caller, std::size_t event) {
        auto owner = source_.events[event].scope;
        const auto common = source_.common_scope(caller, owner);
        while (owner != common) {
            auto & exports = scopes_[owner].exports;
            exports.try_emplace(event, exports.size());
            owner = *source_.scopes[owner].info.parent;
        }
    }

    std::size_t resource(std::size_t source, TemplateDefinition & definition, TemplateFrame & frame) const {
        for (std::size_t index = 0; index < frame.resources.size(); ++index) {
            if (frame.resources[index] == source) return index;
        }
        const auto & descriptor = source_.resources[source];
        const auto index = frame.resources.size();
        frame.resources.push_back(source);
        definition.resources.push_back({ descriptor.kind, descriptor.role, descriptor.serial });
        return index;
    }

    TemplateReference reference(std::size_t scope, std::size_t event, TemplateDefinition & definition, TemplateFrame & frame) {
        const auto owner = source_.events[event].scope;
        if (owner == scope) return { event_positions_[event], std::nullopt, false };
        if (source_.contains(scope, owner)) {
            auto child = owner;
            while (*source_.scopes[child].info.parent != scope) child = *source_.scopes[child].info.parent;
            return { scopes_[child].exports.at(event), child_positions_[child], false };
        }

        auto & imports = scopes_[scope].imports;
        for (std::size_t index = 0; index < imports.size(); ++index) {
            if (imports[index] == event) return { index, {}, true };
        }
        const auto index = imports.size();
        imports.push_back(event);
        definition.imports.push_back(resource(source_.events[event].resource, definition, frame));
        return { index, {}, true };
    }

    TensorBinding tensor_binding(const TensorObservation & tensor) {
        std::optional<std::vector<std::size_t>> writer;
        if (tensor.writer) {
            writer.emplace();
            for (auto scope = *tensor.writer; scope != 0; scope = *source_.scopes[scope].info.parent) writer->push_back(child_positions_[scope]);
            std::reverse(writer->begin(), writer->end());
        }
        return { values_.at(tensor.value),
                 storage_.at(tensor.storage),
                 tensor.version - versions_.at(tensor.storage),
                 external_.contains(tensor.storage),
                 std::move(writer),
                 external_.contains(tensor.storage) ? tensor.buffer : std::string{} };
    }

    void compile_scope(std::size_t scope) {
        auto & compiled = scopes_[scope];
        const auto & observed = source_.scopes[scope];
        TemplateDefinition definition;
        definition.role = observed.info.role;
        definition.implementation = observed.info.implementation;
        for (const auto & [name, value] : observed.info.parameters) definition.parameters.insert(name);
        for (const auto & [name, value] : observed.info.constants) definition.fixed.insert(name);

        auto & frame = compiled.frame;

        // Import identities follow named boundaries, not whichever implementation edge happens to mention them first.
        for (const auto & port : observed.ports) {
            reference(scope, port.event, definition, frame);
            if (port.wait) reference(scope, port.wait->begin, definition, frame);
            if (port.rendezvous) reference(scope, *port.rendezvous, definition, frame);
        }

        std::set<nlohmann::json> predicates;
        for (const auto & predicate : observed.info.guards) {
            if (predicate.evaluate(observed.info.parameters) == 0) throw std::invalid_argument("native guard contradicts source at " + observed.info.site);
            predicates.insert(predicate.document());
        }
        frame.site = observed.info.site;
        frame.communication = observed.communication;
        frame.bindings = observed.info.bindings;
        frame.constants = observed.info.constants;
        for (const auto & access : observed.slot_accesses) {
            if (observed.info.work.at(access.value.members).get<std::vector<std::string>>() != access.value.requests)
                throw std::invalid_argument("slot member binding differs from observation at " + frame.site);
            definition.slot_accesses.push_back({ access.value.pool,
                                                 access.value.field,
                                                 access.value.access,
                                                 access.value.members,
                                                 reference(scope, access.start, definition, frame),
                                                 reference(scope, access.finish, definition, frame),
                                                 access.value.publication });
        }
        for (const auto & access : observed.kv_accesses) {
            if (!access.value.rule) throw std::invalid_argument("KV access has no declared member range rule at " + frame.site);
            if (access.value.rule->evaluate(observed.info.work) != access.value.regions)
                throw std::invalid_argument("declared KV range differs from observation at " + frame.site);
            definition.kv_accesses.push_back({ access.value.pool,
                                               access.value.access,
                                               access.value.slice,
                                               *access.value.rule,
                                               reference(scope, access.start, definition, frame),
                                               reference(scope, access.finish, definition, frame) });
            frame.kv_layers.push_back(access.value.layer);
        }
        for (const auto event : compiled.events) definition.events.push_back(resource(source_.events[event].resource, definition, frame));
        definition.exports.resize(compiled.exports.size());
        for (const auto & [event, position] : compiled.exports) definition.exports[position] = reference(scope, event, definition, frame);
        for (const auto work : compiled.work) {
            const auto & piece = source_.work[work];
            definition.work.push_back({ resource(piece.resource, definition, frame),
                                        piece.kind,
                                        reference(scope, piece.start, definition, frame),
                                        reference(scope, piece.finish, definition, frame) });
        }
        for (const auto edge : compiled.edges) {
            const auto & link = source_.edges[edge];
            definition.edges.push_back({ reference(scope, link.source, definition, frame), reference(scope, link.target, definition, frame), link.kind });
        }

        for (const auto & port : observed.ports) {
            if (port.use == PortUse::Signal && scope != 0) throw std::invalid_argument("cross-action signals must be exposed on the action boundary");
            TemplatePort boundary{ port.name, port.direction, port.use, port.relation, reference(scope, port.event, definition, frame), std::nullopt };
            if (port.wait)
                boundary.wait =
                    StreamWait<TemplateReference>{ resource(port.wait->resource, definition, frame), reference(scope, port.wait->begin, definition, frame) };
            std::optional<TensorBinding> binding;
            if (port.tensor) {
                const auto & tensor = *port.tensor;
                if (!tensor.shape_rule) throw std::invalid_argument("template boundary requires a declared shape rule at " + frame.site + ":" + port.name);
                boundary.tensor = TensorRule{ tensor.dtype, *tensor.shape_rule };
                for (std::size_t axis = 0; axis < tensor.shape.size(); ++axis) {
                    const auto & dimension = (*tensor.shape_rule)[axis];
                    if (dimension.evaluate(observed.info.parameters) != tensor.shape[axis])
                        throw std::invalid_argument("declared boundary shape differs from observation at " + frame.site + ":" + port.name);

                    // Empty dimensions can omit work and child calls; resizing cannot invent the missing structure.
                    if (dimension.parameterized()) predicates.insert(nlohmann::json{ tensor.shape[axis] == 0 ? "eq" : "lt", 0, dimension.document() });
                }
                binding = tensor_binding(tensor);
            }
            if (port.rendezvous) boundary.rendezvous = reference(scope, *port.rendezvous, definition, frame);
            definition.ports.push_back(std::move(boundary));
            frame.tensors.push_back(std::move(binding));
        }

        nlohmann::json guard = { "and", 1 };
        for (const auto & predicate : predicates) guard.push_back(predicate);
        frame.guard = Expression::read(guard);

        for (const auto child : compiled.children) {
            auto & result = scopes_[child];
            const auto & info = source_.scopes[child].info;
            const auto & child_definition = library_.definitions[result.frame.definition];
            TemplateCall call{ library_.call_interface(result.frame), info.arguments, {} };
            for (const auto & parameter : child_definition.parameters) {
                if (child_definition.fixed.contains(parameter)) continue;
                const auto argument = call.arguments.find(parameter);
                if (argument == call.arguments.end()) throw std::invalid_argument("call parameter has no declared relation at " + info.site + ":" + parameter);
                if (argument->second.evaluate(observed.info.parameters) != info.parameters.at(parameter))
                    throw std::invalid_argument("declared call argument differs from observation at " + info.site + ":" + parameter);
            }
            for (const auto imported : result.imports) call.imports.push_back(reference(scope, imported, definition, frame));
            definition.children.push_back(std::move(call));
            frame.children.push_back(std::move(result.frame));
        }

        // Equality uses the normalized structure only; source sites, resources and tensor identities stay in the frame.
        const auto key = definition.document();
        const auto [found, inserted] = shared_.try_emplace(key, library_.definitions.size());
        frame.definition = found->second;
        if (inserted) library_.definitions.push_back(std::move(definition));
    }

    const CoarseGraph & source_;
    TemplateLibrary & library_;
    std::map<nlohmann::json, std::size_t> & shared_;
    std::vector<ScopeCompilation> scopes_;
    std::vector<std::size_t> child_positions_;
    std::vector<std::size_t> event_positions_;
    std::map<std::string, std::int64_t> versions_;
    std::set<std::string> external_;
    std::map<std::string, std::string> values_;
    std::map<std::string, std::string> storage_;
};

} // namespace

TemplateLibrary TemplateCompiler::compile(const CoarseGraph & source, std::span<const EntrySpecification> entries, nlohmann::json domain) {
    source.validate();
    TemplateLibrary library;
    library.domain = std::move(domain);
    library.resources = source.resources;
    std::map<nlohmann::json, std::size_t> shared;
    std::map<std::tuple<std::string, nlohmann::json, nlohmann::json>, std::size_t> shared_entries;
    const EntryExtraction extraction(source);
    for (const auto & specification : entries) {
        const auto cut = extraction.extract(specification.scope);
        auto entry = LibraryCompilation(cut, library, shared).compile(specification);
        auto key = std::tuple{ entry.action, entry.condition, entry.frame.document() };
        const auto [found, inserted] = shared_entries.try_emplace(std::move(key), library.entries.size());
        if (inserted) library.entries.push_back(std::move(entry));
        else library.entries[found->second].observations.merge(entry.observations);
    }
    library.validate();
    return library;
}

} // namespace markov
