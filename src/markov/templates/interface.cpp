#include "markov/templates/library.hpp"

#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace markov {
namespace {

class InterfaceConstruction {
public:
    InterfaceConstruction(const TemplateLibrary & library, const TemplateFrame & frame) : library_(library) {
        const auto & definition = library.definitions.at(frame.definition);
        result_ = { definition.role, definition.implementation, definition.parameters, definition.fixed, {}, {}, {}, {}, nlohmann::json::array(), {} };
        for (std::size_t imported = 0; imported < definition.imports.size(); ++imported) result_.imports.push_back(boundary(frame, { imported, {}, true }));
        for (const auto & exported : definition.exports) result_.exports.push_back(boundary(frame, exported));
        for (const auto & port : definition.ports) {
            auto contract = port.document();
            contract["event"] = boundary(frame, port.event);
            if (port.rendezvous) contract["rendezvous"] = boundary(frame, *port.rendezvous);
            if (port.wait)
                contract["wait"] = {
                    { "resource", resource(frame.resources.at(port.wait->resource)) },
                    { "begin", boundary(frame, port.wait->begin) }
                };
            result_.ports.push_back(std::move(contract));
        }

        for (const auto & work : definition.work) connect(event(frame, work.start), event(frame, work.finish));
        for (const auto & edge : definition.edges) connect(event(frame, edge.source), event(frame, edge.target));
        for (std::size_t child = 0; child < definition.children.size(); ++child) {
            const auto & call = definition.children[child];
            std::map<std::size_t, std::size_t> visible;
            for (std::size_t imported = 0; imported < call.imports.size(); ++imported)
                visible.emplace(call.contract.imports[imported], event(frame, call.imports[imported]));
            for (std::size_t exported = 0; exported < call.contract.exports.size(); ++exported)
                visible.emplace(call.contract.exports[exported], event(frame, { exported, child, false }));

            // A parent composes the child's boundary guarantees without inspecting its internal work or edges.
            for (const auto & [source, target] : call.contract.causality) {
                const auto from = visible.find(source), to = visible.find(target);
                if (from != visible.end() && to != visible.end()) connect(from->second, to->second);
            }
        }

        // Publish reachability only between public boundaries; internal event counts and paths remain private.
        for (std::size_t origin = 0; origin < result_.boundaries.size(); ++origin) {
            std::vector<bool> reached(successors_.size());
            auto pending = successors_[origin];
            while (!pending.empty()) {
                const auto current = pending.back();
                pending.pop_back();
                if (reached[current]) continue;
                reached[current] = true;
                pending.insert(pending.end(), successors_[current].begin(), successors_[current].end());
            }
            for (std::size_t target = 0; target < result_.boundaries.size(); ++target) {
                if (reached[target]) result_.causality.emplace_back(origin, target);
            }
        }
    }

    TemplateInterface take() { return std::move(result_); }

private:
    std::size_t boundary(const TemplateFrame & frame, const TemplateReference & reference) {
        const auto & definition = library_.definitions.at(frame.definition);
        if (reference.child) {
            const auto & child = frame.children.at(*reference.child);
            return boundary(child, library_.definitions.at(child.definition).exports.at(reference.event));
        }

        // Boundary aliases and named port meanings survive internal event renumbering or different child bodies.
        const auto identity = event(frame, reference);
        if (identity == result_.boundaries.size()) {
            const auto slot = reference.imported ? definition.imports.at(reference.event) : definition.events.at(reference.event);
            result_.boundaries.push_back(resource(frame.resources.at(slot)));
        }
        return identity;
    }

    std::size_t event(const TemplateFrame & frame, const TemplateReference & reference) {
        if (reference.child) {
            const auto & child = frame.children.at(*reference.child);
            return event(child, library_.definitions.at(child.definition).exports.at(reference.event));
        }
        const auto [found, inserted] = endpoints_.try_emplace(std::tuple{ &frame, reference.imported, reference.event }, endpoints_.size());
        if (inserted) successors_.emplace_back();
        return found->second;
    }

    void connect(std::size_t source, std::size_t target) { successors_[source].push_back(target); }

    std::size_t resource(std::size_t identity) {
        const auto [found, inserted] = resources_.try_emplace(identity, resources_.size());
        if (inserted) {
            const auto & actual = library_.resources.at(identity);
            result_.resources.push_back({ actual.kind, actual.role, actual.serial });
        }
        return found->second;
    }

    const TemplateLibrary & library_;
    TemplateInterface result_;
    std::map<std::size_t, std::size_t> resources_;
    std::map<std::tuple<const TemplateFrame *, bool, std::size_t>, std::size_t> endpoints_;
    std::vector<std::vector<std::size_t>> successors_;
};

} // namespace

TemplateInterface TemplateLibrary::call_interface(const TemplateFrame & frame) const { return InterfaceConstruction(*this, frame).take(); }

} // namespace markov
