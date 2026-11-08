#include "markov/templates/selection.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace markov {
namespace {

struct FrameChoice {
    std::optional<TemplateFrame> frame;
    std::optional<std::string> ambiguity;
    std::optional<std::string> rejected_guard;

    void merge(FrameChoice candidate, std::string_view site) {
        if (!rejected_guard) rejected_guard = std::move(candidate.rejected_guard);
        if (candidate.ambiguity) {
            frame.reset();
            ambiguity = std::move(candidate.ambiguity);
        }
        else if (candidate.frame) {
            if (frame || ambiguity) {
                frame.reset();
                if (!ambiguity) ambiguity = std::string(site);
            }
            else frame = std::move(candidate.frame);
        }
    }
};

using FrameFamily = std::vector<const TemplateFrame *>;

class FamilyIndex {
public:
    FamilyIndex(const TemplateLibrary & library, std::vector<FrameFamily> & families, std::map<const TemplateFrame *, std::size_t> & membership,
                std::deque<TemplateFrame> & alternatives)
        : library_(library),
          families_(families),
          membership_(membership),
          alternatives_(alternatives) {
        for (const auto & implementation : library.implementations) implementations_[implementation.contract].push_back(&implementation);
    }

    std::size_t add(const TemplateEntry & entry, const TemplateFrame & frame, const std::string & space = "scope/", std::optional<std::size_t> caller = {},
                    std::vector<std::size_t> position = {}) {
        for (std::size_t child = 0; child < frame.children.size(); ++child) {
            position.push_back(child);
            add(entry, frame.children[child], space + std::to_string(child) + "/", frame.definition, position);
            position.pop_back();
        }
        auto local = frame.local_document();
        local.erase("definition");
        local.erase("guard");
        local["interface"] = library_.call_interface(frame).document();
        std::set<nlohmann::json> shared;
        std::map<std::string, Expression> arguments;
        for (const auto & parameter : library_.definitions[frame.definition].parameters) arguments.emplace(parameter, Expression::read(parameter));
        for (const auto & [name, value] : frame.constants) arguments.insert_or_assign(name, Expression::read(value));
        foreign_bindings(frame, space, space, arguments, shared);
        local["shared_bindings"] = shared;
        const auto [found, inserted] = keys_.try_emplace(std::tuple{ entry.action, entry.condition, space, caller, std::move(local) }, families_.size());
        if (inserted) families_.emplace_back();
        families_[found->second].push_back(&frame);

        const auto contract = library_.local_contract(frame, position);
        if (!contract.is_null()) {
            for (const auto * implementation : implementations_.at(contract)) {

                // Rebind only the observed body and its guard; the recipient retains all caller and rank identities.
                auto replacement = frame;
                replacement.definition = implementation->definition;
                replacement.guard = implementation->guard;
                if (library_.local_contract(replacement, position) != contract)
                    throw std::invalid_argument("local implementation changes the recipient contract");
                alternatives_.push_back(std::move(replacement));
                families_[found->second].push_back(&alternatives_.back());
            }
        }
        membership_.emplace(&frame, found->second);
        return found->second;
    }

private:
    void foreign_bindings(const TemplateFrame & frame, std::string_view owner, const std::string & position,
                          const std::map<std::string, Expression> & arguments, std::set<nlohmann::json> & shared) const {
        const auto & definition = library_.definitions[frame.definition];
        for (std::size_t port = 0; port < frame.tensors.size(); ++port) {
            const auto & tensor = frame.tensors[port];
            if (!tensor || (tensor->value.starts_with(owner) && tensor->storage.starts_with(owner))) continue;
            const auto & boundary = definition.ports[port];
            auto meaning = boundary.document();
            meaning.erase("event");
            meaning["tensor"]["shape"] = nlohmann::json::array();
            for (const auto & dimension : boundary.tensor->shape) meaning["tensor"]["shape"].push_back(dimension.substitute(arguments).document());

            // A replacement cannot change another scope's aliases, versions, writers or data access contract.
            shared.insert(nlohmann::json{
                { "position",                                                                   position },
                {     "port",                                                         std::move(meaning) },
                {    "value",                                                              tensor->value },
                {  "storage",                                                            tensor->storage },
                {  "version",                                                            tensor->version },
                { "external",                                                           tensor->external },
                {   "buffer",                                                             tensor->buffer },
                {   "writer", tensor->writer ? nlohmann::json(*tensor->writer) : nlohmann::json(nullptr) }
            });
        }
        for (std::size_t child = 0; child < frame.children.size(); ++child) {
            std::map<std::string, Expression> parameters;
            for (const auto & [name, value] : frame.children[child].constants) parameters.emplace(name, Expression::read(value));
            for (const auto & [name, expression] : definition.children[child].arguments) parameters.emplace(name, expression.substitute(arguments));
            foreign_bindings(frame.children[child], owner, position + std::to_string(child) + "/", parameters, shared);
        }
    }

    const TemplateLibrary & library_;
    std::vector<FrameFamily> & families_;
    std::map<const TemplateFrame *, std::size_t> & membership_;
    std::deque<TemplateFrame> & alternatives_;
    std::map<nlohmann::json, std::vector<const LocalImplementation *>> implementations_;
    std::map<std::tuple<std::string, nlohmann::json, std::string, std::optional<std::size_t>, nlohmann::json>, std::size_t> keys_;
};

class FrameSelection {
public:
    FrameSelection(const TemplateLibrary & library, const std::vector<FrameFamily> & families, const std::map<const TemplateFrame *, std::size_t> & membership)
        : library_(library),
          families_(families),
          membership_(membership) {}

    FrameChoice resolve(std::span<const TemplateFrame * const> candidates, const Parameters & arguments) const {
        Parameters parameters = candidates.front()->constants;
        for (const auto & [name, value] : arguments) {
            const auto [previous, inserted] = parameters.emplace(name, value);
            if (!inserted && previous->second != value) return {};
        }

        // Family construction fixes the interface and constants; only guards and child choices vary at runtime.
        const auto & expected = library_.definitions[candidates.front()->definition].parameters;
        if (expected.size() != parameters.size() || !std::ranges::all_of(expected, [&](const auto & name) { return parameters.contains(name); }))
            return {};

        const TemplateFrame * rejected = nullptr;
        std::map<std::pair<std::size_t, std::vector<std::size_t>>, const TemplateFrame *> implementations;
        for (const auto * candidate : candidates) {
            if (candidate->guard.evaluate(parameters) == 0) {
                if (!rejected) rejected = candidate;
                continue;
            }
            std::vector<std::size_t> children;
            for (const auto & child : candidate->children) children.push_back(membership_.at(&child));
            implementations.try_emplace(std::pair{ candidate->definition, std::move(children) }, candidate);
        }

        FrameChoice result;
        if (implementations.empty() && rejected) {
            Parameters relevant;
            for (const auto & name : rejected->guard.variables()) relevant.emplace(name, parameters.at(name));

            // Keep one actual rejection, not an invented explanation for every candidate in the family.
            result.rejected_guard = nlohmann::json{
                { "site", rejected->site }, { "definition", rejected->definition },
                { "guard", rejected->guard.document() }, { "parameters", relevant }
            }.dump();
        }
        for (const auto & [choice, candidate] : implementations) {
            const auto & [identity, child_families] = choice;
            const auto & source = *candidate;
            const auto & body = library_.definitions[identity];

            // Keep this observed caller body; only compatible child implementations are independently selected.
            FrameChoice chosen{
                TemplateFrame{ identity,
                              source.guard,
                              source.site,
                              source.bindings,
                              source.constants,
                              source.resources,
                              source.tensors,
                              {},
                              source.kv_layers,
                              source.communication },
                {},
                {}
            };
            for (std::size_t child = 0; child < body.children.size(); ++child) {
                const auto & call = body.children[child];
                Parameters child_arguments;
                for (const auto & [name, expression] : call.arguments) child_arguments.emplace(name, expression.evaluate(parameters));
                auto selected = resolve(families_[child_families[child]], child_arguments);
                if (!selected.frame && !selected.ambiguity) {
                    chosen = std::move(selected);
                    break;
                }
                if (selected.ambiguity) chosen.ambiguity = std::move(selected.ambiguity);
                else chosen.frame->children.push_back(std::move(*selected.frame));
            }
            if (chosen.ambiguity) chosen.frame.reset();
            result.merge(std::move(chosen), source.site);
        }
        return result;
    }

private:
    const TemplateLibrary & library_;
    const std::vector<FrameFamily> & families_;
    const std::map<const TemplateFrame *, std::size_t> & membership_;
};

} // namespace

TemplateSelection::TemplateSelection(const TemplateLibrary & library) : library_(library) {
    FamilyIndex index(library_, families_, membership_, alternatives_);
    for (const auto & entry : library.entries) roots_[{ entry.action, entry.condition }].insert(index.add(entry, entry.frame));
}

TemplateEntry TemplateSelection::select(std::string_view action, const nlohmann::json & condition, const Parameters & parameters) const {
    const auto roots = roots_.find({ std::string(action), condition });
    if (roots == roots_.end())
        throw std::invalid_argument("template action has no declared candidates: " + std::string(action) + " condition=" + condition.dump());
    FrameSelection selection(library_, families_, membership_);
    FrameChoice chosen;
    for (const auto family : roots->second) chosen.merge(selection.resolve(families_[family], parameters), action);
    if (chosen.ambiguity)
        throw std::invalid_argument("ambiguous local template implementation at " + *chosen.ambiguity + " action=" + std::string(action)
                                    + " condition=" + condition.dump() + " parameters=" + nlohmann::json(parameters).dump());
    if (!chosen.frame)
        throw std::invalid_argument("template action has no matching local conditions: " + std::string(action) + " condition=" + condition.dump()
                                    + (chosen.rejected_guard ? " rejected_local_candidate=" + *chosen.rejected_guard
                                                             : " parameters=" + nlohmann::json(parameters).dump()));

    // A composed target is not an observed whole call; source observations remain on the stored candidates.
    return { std::string(action), condition, std::move(*chosen.frame), {} };
}

} // namespace markov
