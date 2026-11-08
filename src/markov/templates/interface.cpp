#include "markov/templates/library.hpp"

#include <map>
#include <tuple>
#include <utility>

namespace markov {
namespace {

class InterfaceConstruction {
public:
    InterfaceConstruction(const TemplateLibrary & library, const TemplateFrame & frame) : library_(library) {
        const auto & definition = library.definitions.at(frame.definition);
        result_ = { definition.role, definition.implementation, definition.parameters, definition.fixed, {}, {}, {}, {}, nlohmann::json::array() };
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
        const auto [found, inserted] = endpoints_.try_emplace(std::tuple{ &frame, reference.imported, reference.event }, endpoints_.size());
        if (inserted) {
            const auto slot = reference.imported ? definition.imports.at(reference.event) : definition.events.at(reference.event);
            result_.boundaries.push_back(resource(frame.resources.at(slot)));
        }
        return found->second;
    }

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
};

} // namespace

TemplateInterface TemplateLibrary::call_interface(const TemplateFrame & frame) const { return InterfaceConstruction(*this, frame).take(); }

} // namespace markov
