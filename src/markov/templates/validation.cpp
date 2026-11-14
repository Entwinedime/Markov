#include "markov/templates/library.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "markov/templates/selection.hpp"

namespace markov {
namespace {

void variables(const Expression & expression, const std::set<std::string> & parameters) {
    for (const auto & name : expression.variables()) {
        if (!parameters.contains(name)) throw std::invalid_argument("template expression uses an undeclared parameter: " + name);
    }
}

void reference(const TemplateDefinition & owner, const TemplateReference & endpoint) {
    if (endpoint.imported) {
        if (endpoint.child || endpoint.event >= owner.imports.size()) throw std::invalid_argument("invalid imported template endpoint");
        return;
    }
    if (endpoint.child) {
        const auto & callee = owner.children.at(*endpoint.child).contract;
        if (endpoint.event >= callee.exports.size()) throw std::invalid_argument("undefined child interface endpoint");
    }
    else if (endpoint.event >= owner.events.size()) throw std::invalid_argument("undefined local template endpoint");
}

void frame(const TemplateLibrary & library, const TemplateFrame & bindings, const TemplateFrame & root, std::map<std::string, std::string> & buffers) {
    const auto & definition = library.definitions.at(bindings.definition);
    variables(bindings.guard, definition.parameters);
    if (!bindings.bindings.is_object() || bindings.resources.size() != definition.resources.size() || bindings.tensors.size() != definition.ports.size()
        || bindings.children.size() != definition.children.size() || bindings.constants.size() != definition.fixed.size()
        || bindings.kv_layers.size() != definition.kv_accesses.size())
        throw std::invalid_argument("template frame does not match its definition");

    if (bindings.communication) {
        bindings.communication->validate();
        for (const auto & field : bindings.communication->agreements) {
            const auto found = std::ranges::find_if(definition.ports, [&](const auto & port) {
                return port.name == field.port && port.direction == field.direction;
            });
            if (found == definition.ports.end() || !found->tensor || (found->use != PortUse::Data && found->use != PortUse::Metadata))
                throw std::invalid_argument("template communication payload agreement requires a tensor port");
        }
    }
    const auto rendezvous = std::ranges::count_if(definition.ports, [](const auto & port) { return port.rendezvous.has_value(); });
    if (rendezvous != (bindings.communication ? 1 : 0)) throw std::invalid_argument("template communication binding differs from its ports");
    for (const auto & name : definition.fixed) bindings.constants.at(name);
    for (const auto layer : bindings.kv_layers) {
        if (layer < 0) throw std::invalid_argument("KV layer must be nonnegative");
    }
    for (std::size_t index = 0; index < bindings.resources.size(); ++index) {
        const auto & resource = library.resources.at(bindings.resources[index]);
        const auto & slot = definition.resources[index];
        if (resource.kind != slot.kind || resource.role != slot.role || resource.serial != slot.serial)
            throw std::invalid_argument("template frame binds an incompatible resource");
    }
    for (std::size_t port = 0; port < definition.ports.size(); ++port) {
        if (bindings.tensors[port].has_value() != definition.ports[port].tensor.has_value())
            throw std::invalid_argument("template tensor binding is missing or unexpected");
        if (bindings.tensors[port]) {
            const auto & tensor = *bindings.tensors[port];
            if (tensor.value.empty() || tensor.storage.empty() || tensor.version < 0) throw std::invalid_argument("invalid template tensor binding");
            if (!tensor.buffer.empty()) {
                if (!tensor.external) throw std::invalid_argument("persistent buffer cannot be an invocation-local allocation");
                const auto [found, inserted] = buffers.emplace(tensor.buffer, tensor.storage);
                if (!inserted && found->second != tensor.storage)
                    throw std::invalid_argument("persistent buffer name refers to different template storages");
                for (const auto & [name, storage] : buffers) {
                    if (storage == tensor.storage && name != tensor.buffer)
                        throw std::invalid_argument("template storage has conflicting persistent owners");
                }
            }
            if (tensor.writer) {
                const auto * owner = &root;
                for (const auto child : *tensor.writer) owner = &owner->children.at(child);
            }
        }
    }
    for (std::size_t child = 0; child < definition.children.size(); ++child) {
        frame(library, bindings.children[child], root, buffers);
        if (library.call_interface(bindings.children[child]) != definition.children[child].contract)
            throw std::invalid_argument("child implementation differs from the caller interface");
    }
}

void observed_frame(const TemplateLibrary & library, const TemplateFrame & bindings, Parameters parameters) {
    for (const auto & [name, value] : bindings.constants) {
        const auto [previous, inserted] = parameters.emplace(name, value);
        if (!inserted && previous->second != value) throw std::invalid_argument("observed call changes a fixed template parameter");
    }
    if (bindings.guard.evaluate(parameters) == 0) throw std::invalid_argument("local template guard contradicts its source at " + bindings.site);
    const auto & definition = library.definitions[bindings.definition];
    for (std::size_t child = 0; child < definition.children.size(); ++child) {
        Parameters arguments;
        for (const auto & [name, expression] : definition.children[child].arguments) arguments.emplace(name, expression.evaluate(parameters));
        observed_frame(library, bindings.children[child], std::move(arguments));
    }
}

} // namespace

void TemplateLibrary::validate() const {
    if (!domain.is_object() || domain.empty() || entries.empty())
        throw std::invalid_argument("template library requires an implementation domain and action entries");

    if (!implementations.empty() && (!local_domain.is_object() || local_domain.empty()))
        throw std::invalid_argument("local implementations require an adapter-declared environment domain");
    for (const auto & local : implementations) {
        const auto & definition = definitions.at(local.definition);
        if (!local.contract.is_object() || !definition.children.empty() || !definition.kv_accesses.empty() || !definition.slot_accesses.empty())
            throw std::invalid_argument("local implementation must be a closed leaf without request state");
        variables(local.guard, definition.parameters);
    }

    for (std::size_t identity = 0; identity < definitions.size(); ++identity) {
        const auto & definition = definitions[identity];
        if (definition.role.empty() || !std::ranges::includes(definition.parameters, definition.fixed))
            throw std::invalid_argument("invalid template role or fixed parameters");
        for (const auto & resource : definition.resources) {
            if (resource.role.empty() || (resource.kind != ResourceKind::Service && !resource.serial))
                throw std::invalid_argument("invalid template resource slot");
        }
        for (const auto resource : definition.events) definition.resources.at(resource);
        for (const auto resource : definition.imports) definition.resources.at(resource);

        // Shared callers constrain interfaces; the finite frame tree owns concrete implementation choices.
        for (const auto & child : definition.children) {
            const auto & callee = child.contract;
            if (callee.role.empty() || !std::ranges::includes(callee.parameters, callee.fixed))
                throw std::invalid_argument("invalid child interface role or fixed parameters");
            for (const auto slot : callee.boundaries) callee.resources.at(slot);
            for (const auto endpoint : callee.imports) callee.boundaries.at(endpoint);
            for (const auto endpoint : callee.exports) callee.boundaries.at(endpoint);
            if (child.imports.size() != callee.imports.size() || child.arguments.size() != callee.parameters.size() - callee.fixed.size())
                throw std::invalid_argument("template call arguments or imports are incomplete");
            for (const auto & [name, expression] : child.arguments) {
                if (!callee.parameters.contains(name) || callee.fixed.contains(name))
                    throw std::invalid_argument("template call overrides a fixed or unknown parameter");
                variables(expression, definition.parameters);
            }
        }

        for (const auto & piece : definition.work) {
            definition.resources.at(piece.resource);
            if (piece.kind == WorkKind::Wait || piece.kind == WorkKind::Boundary) throw std::invalid_argument("template charges a wait or pure boundary");
            reference(definition, piece.start);
            reference(definition, piece.finish);
        }
        for (const auto & endpoint : definition.exports) {

            // Export resolution descends only; an imported alias could create a caller/callee reference cycle.
            if (endpoint.imported) throw std::invalid_argument("template cannot re-export an imported endpoint");
            reference(definition, endpoint);
        }
        for (const auto & edge : definition.edges) {
            reference(definition, edge.source);
            reference(definition, edge.target);
        }
        for (const auto & access : definition.slot_accesses) {
            if (access.pool.empty() || access.field.empty() || access.members.empty())
                throw std::invalid_argument("template slot access has an incomplete binding");
            if (!access.publication.empty() && access.access != StateAccess::Write)
                throw std::invalid_argument("only template slot writes can declare a replicated publication");
            reference(definition, access.start);
            reference(definition, access.finish);
        }
        for (const auto & access : definition.kv_accesses) {
            if (access.pool.empty()) throw std::invalid_argument("template KV access has no pool binding");
            access.slice.validate();
            reference(definition, access.start);
            reference(definition, access.finish);
        }

        std::set<std::pair<Direction, std::string>> ports;
        for (const auto & port : definition.ports) {
            if (port.name.empty() || !ports.emplace(port.direction, port.name).second)
                throw std::invalid_argument("template port has an empty or duplicate name");
            reference(definition, port.event);
            if (port.rendezvous) {
                reference(definition, *port.rendezvous);
                if (port.direction != Direction::Output || port.use != PortUse::Control || port.relation != Relation::Communication || port.tensor || port.wait)
                    throw std::invalid_argument("invalid template communication completion port");
            }
            if (port.wait) {
                if (port.direction != Direction::Input || port.use != PortUse::Control || port.relation != Relation::Signal || port.tensor
                    || definition.resources.at(port.wait->resource).kind != ResourceKind::Stream)
                    throw std::invalid_argument("invalid template stream wait port");
                reference(definition, port.wait->begin);
            }
            if (port.use == PortUse::Data && !port.tensor) throw std::invalid_argument("template data ports require a tensor");
            if (port.use == PortUse::Signal && (port.relation != Relation::Signal || port.tensor || port.wait || port.rendezvous))
                throw std::invalid_argument("template completion signal must be a pure signal boundary");
            if (port.tensor) {
                if (port.tensor->dtype.empty()) throw std::invalid_argument("template tensor has no dtype");
                for (const auto & dimension : port.tensor->shape) variables(dimension, definition.parameters);
            }
        }
        for (const auto & child : definition.children) {
            for (const auto & imported : child.imports) reference(definition, imported);
        }
    }
    for (const auto & entry : entries) {
        if (entry.action.empty() || !entry.condition.is_object() || entry.frame.definition >= definitions.size()
            || !definitions[entry.frame.definition].imports.empty())
            throw std::invalid_argument("template action requires a name, condition and closed definition");
        std::map<std::string, std::string> buffers;
        frame(*this, entry.frame, entry.frame, buffers);
        for (const auto & parameters : entry.observations) observed_frame(*this, entry.frame, parameters);
    }

    // A published library must select one implementation for its own observations, before any serving run needs it.
    const TemplateSelection selection(*this);
    std::set<std::tuple<std::string, nlohmann::json, Parameters>> checked;
    for (const auto & entry : entries) {
        for (const auto & parameters : entry.observations) {
            if (checked.emplace(entry.action, entry.condition, parameters).second)
                selection.select(entry.action, entry.condition, parameters);
        }
    }
}

} // namespace markov
