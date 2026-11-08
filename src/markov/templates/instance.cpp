#include "markov/templates/library.hpp"

#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov {
namespace {

struct InstanceScope {
    const TemplateFrame * frame;
    std::optional<std::size_t> parent;
    std::size_t call;
    std::vector<std::size_t> events;
    std::vector<std::size_t> children;
};

class GraphInstantiation {
public:
    GraphInstantiation(const TemplateLibrary & library, const TemplateEntry & entry, const GraphInvocation & invocation)
        : library_(library),
          entry_(entry),
          invocation_(invocation) {
        if (invocation.identity.empty() || !invocation.work.is_object() || invocation.resources.size() != library.resources.size())
            throw std::invalid_argument("graph invocation requires an identity, work and explicit resource bindings");
        if (!invocation.environment.is_object()) throw std::invalid_argument("invocation requires its target environment");
        external_storage(entry.frame);
        if (external_.size() != invocation.storage.size()) throw std::invalid_argument("invocation has unknown external storage bindings");
        std::set<std::string> signals;
        for (const auto & port : library.definitions[entry.frame.definition].ports) {
            if (port.use != PortUse::Signal) continue;
            if (invocation.signals.at(port.name).empty()) throw std::invalid_argument("target completion signal has an empty identity");
            signals.insert(port.name);
        }
        if (signals.size() != invocation.signals.size()) throw std::invalid_argument("invocation has unknown completion signal bindings");
        graph_.environment = invocation.environment;
        graph_.resources = invocation.resources;
    }

    CoarseGraph instantiate() {
        append(entry_.frame, std::nullopt, 0, invocation_.parameters, {});

        // Allocate all local boundaries before resolving cross-scope references; no event is copied or guessed.
        for (std::size_t scope = 0; scope < scopes_.size(); ++scope) {
            const auto definition_id = scopes_[scope].frame->definition;
            const auto * bindings = scopes_[scope].frame;
            const auto & definition = library_.definitions[definition_id];
            for (std::size_t child = 0; child < definition.children.size(); ++child) {
                const auto & call = definition.children[child];
                Parameters arguments;
                for (const auto & [name, expression] : call.arguments) arguments.emplace(name, expression.evaluate(graph_.scopes[scope].info.parameters));

                const auto identity = scopes_.size();
                scopes_[scope].children.push_back(identity);
                append(bindings->children[child], scope, child, arguments, call.arguments);
            }
        }
        for (std::size_t scope = 0; scope < scopes_.size(); ++scope) body(scope);
        graph_.validate();
        return std::move(graph_);
    }

private:
    void external_storage(const TemplateFrame & frame) {
        for (const auto & tensor : frame.tensors) {
            if (!tensor || !tensor->external) continue;
            const auto & target = invocation_.storage.at(tensor->storage);
            if (target.identity.empty() || target.version < 0) throw std::invalid_argument("invalid target storage identity or version");
            external_.insert(tensor->storage);
        }
        for (const auto & child : frame.children) external_storage(child);
    }

    void append(const TemplateFrame & frame, std::optional<std::size_t> parent, std::size_t call, const Parameters & arguments,
                const std::map<std::string, Expression> & relations) {
        const auto & definition = library_.definitions[frame.definition];
        Parameters parameters = frame.constants;
        for (const auto & [name, value] : arguments) {
            if (!definition.parameters.contains(name) || definition.fixed.contains(name))
                throw std::invalid_argument("invocation overrides a fixed or unknown parameter: " + name);
            parameters.emplace(name, value);
        }
        if (parameters.size() != definition.parameters.size()) throw std::invalid_argument("invocation has missing dynamic parameters at " + frame.site);

        if (frame.guard.evaluate(parameters) == 0) throw std::invalid_argument("target parameters do not satisfy the local template guard at " + frame.site);

        const auto identity = scopes_.size();
        graph_.scopes.push_back({
            ScopeInfo{ parent,
                      definition.role,
                      frame.site,
                      definition.implementation,
                      std::move(parameters),
                      frame.constants,
                      relations, invocation_.work,
                      frame.bindings },
            {}
        });
        graph_.scopes.back().info.guards = { frame.guard };
        graph_.scopes.back().communication = frame.communication;
        InstanceScope node{ &frame, parent, call, {}, {} };
        for (std::size_t slot = 0; slot < definition.resources.size(); ++slot) {
            const auto & actual = graph_.resources.at(frame.resources[slot]);
            const auto & expected = definition.resources[slot];
            if (actual.kind != expected.kind || actual.role != expected.role || actual.serial != expected.serial)
                throw std::invalid_argument("invocation binds an incompatible execution resource");
        }
        for (const auto resource : definition.events) {
            node.events.push_back(graph_.events.size());
            graph_.events.push_back({ identity, frame.resources[resource] });
        }
        scopes_.push_back(std::move(node));
    }

    std::size_t event(std::size_t scope, const TemplateReference & reference) const {
        if (reference.imported) {
            const auto & node = scopes_[scope];
            const auto parent = *node.parent;
            const auto & caller = library_.definitions[scopes_[parent].frame->definition];
            return event(parent, caller.children[node.call].imports[reference.event]);
        }
        if (reference.child) {
            const auto child = scopes_[scope].children[*reference.child];
            return event(child, library_.definitions[scopes_[child].frame->definition].exports[reference.event]);
        }
        return scopes_[scope].events[reference.event];
    }

    void body(std::size_t scope) {
        const auto & node = scopes_[scope];
        const auto & frame = *node.frame;
        const auto & definition = library_.definitions[frame.definition];
        for (const auto & access : definition.slot_accesses) {
            graph_.scopes[scope].slot_accesses.push_back({
                { invocation_.pools.at(access.pool),
                 access.field,
                 access.access,
                 access.members,
                 invocation_.work.at(access.members).get<std::vector<std::string>>() },
                event(scope, access.start),
                event(scope, access.finish)
            });
        }
        for (std::size_t index = 0; index < definition.kv_accesses.size(); ++index) {
            const auto & access = definition.kv_accesses[index];
            graph_.scopes[scope].kv_accesses.push_back({
                { invocation_.pools.at(access.pool),
                 frame.kv_layers[index],
                 access.access,
                 access.slice,
                 access.rule.evaluate(graph_.scopes[scope].info.work),
                 access.rule },
                event(scope, access.start),
                event(scope, access.finish)
            });
        }
        for (const auto & piece : definition.work)
            graph_.work.push_back({ scope, frame.resources[piece.resource], piece.kind, event(scope, piece.start), event(scope, piece.finish) });
        for (const auto & edge : definition.edges) graph_.edges.push_back({ event(scope, edge.source), event(scope, edge.target), edge.kind });

        for (std::size_t port = 0; port < definition.ports.size(); ++port) {
            const auto & description = definition.ports[port];
            GraphPort boundary{ description.name, description.direction, description.use, description.relation, event(scope, description.event), std::nullopt };
            if (description.wait) boundary.wait = StreamWait<std::size_t>{ frame.resources[description.wait->resource], event(scope, description.wait->begin) };
            if (description.rendezvous) boundary.rendezvous = event(scope, *description.rendezvous);
            if (description.use == PortUse::Signal) {
                if (scope != 0) throw std::invalid_argument("completion signal is not exposed on the action boundary");
                boundary.signal = invocation_.signals.at(description.name);
            }
            if (description.tensor) {
                const auto & binding = *frame.tensors[port];

                auto storage = invocation_.identity + "/" + binding.storage;
                auto version = binding.version;
                if (binding.external) {
                    const auto & target = invocation_.storage.at(binding.storage);
                    storage = target.identity;
                    if (version > std::numeric_limits<std::int64_t>::max() - target.version) throw std::invalid_argument("target tensor version overflows");
                    version += target.version;
                }

                // Local values are fresh; existing allocations and epochs come only from the target resource owner.
                auto layout = description.tensor->evaluate(graph_.scopes[scope].info.parameters);
                TensorObservation tensor{ invocation_.identity + "/" + binding.value,
                                          std::move(storage),
                                          version,
                                          std::move(layout.dtype),
                                          std::move(layout.shape),
                                          description.tensor->shape };
                if (!binding.external) tensor.allocation = 0;
                tensor.buffer = binding.buffer;
                if (binding.writer) {
                    std::size_t writer = 0;
                    for (const auto child : *binding.writer) writer = scopes_[writer].children[child];
                    tensor.writer = writer;
                }
                boundary.tensor = std::move(tensor);
            }
            graph_.scopes[scope].ports.push_back(std::move(boundary));
        }
    }

    const TemplateLibrary & library_;
    const TemplateEntry & entry_;
    const GraphInvocation & invocation_;
    CoarseGraph graph_;
    std::vector<InstanceScope> scopes_;
    std::set<std::string> external_;
};

} // namespace

CoarseGraph TemplateLibrary::instantiate(const TemplateEntry & entry, const GraphInvocation & invocation) const {
    if (invocation.domain != domain) throw std::invalid_argument("target implementation domain differs from the template source");
    return GraphInstantiation(*this, entry, invocation).instantiate();
}

} // namespace markov
