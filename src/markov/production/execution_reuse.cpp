#include "markov/production/execution_source.hpp"

#include <algorithm>
#include <optional>

namespace markov {

nlohmann::json ExecutionSource::context_bindings(const nlohmann::json & resources) const {
    auto result = resources;
    for (auto & value : result) {
        const auto identity = value.get<std::size_t>();
        if (kinds_.at(identity) == "event" && !external_events_.contains(identity)) value = nullptr;
    }
    return result;
}

std::map<nlohmann::json, std::set<nlohmann::json>> ExecutionSource::anchors() const {
    std::map<nlohmann::json, std::set<nlohmann::json>> result;

    // External event identities are known only after the complete source has been observed.
    for (const auto & [key, bindings] : anchors_) {
        for (const auto & binding : bindings) result[key].insert(context_bindings(binding));
    }
    return result;
}

class ExecutionSource::Reuse {
public:
    Reuse(std::span<ExecutionSource> sources, nlohmann::json & roles) : sources_(sources), roles_(roles) {
        for (std::size_t index = 0; index < sources.size(); ++index) {
            anchors_.push_back(sources[index].anchors());
            variants_.push_back(roles[index].at("execution_variants"));
        }
    }

    void publish() {
        for (std::size_t target = 0; target < sources_.size(); ++target) {
            for (auto & selection : roles_[target]["execution_variants"]) {
                if (!sources_[target].domain_.is_null()) extend(target, selection);
                selection.erase("literals");
            }
        }
    }

private:
    using Mapping = std::map<std::size_t, std::size_t>;
    using Anchors = std::map<nlohmann::json, std::set<nlohmann::json>>;

    static nlohmann::json contract(const nlohmann::json & selection) {
        auto result = selection;
        for (const auto * field : { "caller", "context", "choices" }) result.erase(field);
        std::sort(result["conditions"].begin(), result["conditions"].end());
        return result;
    }

    static bool covers(const nlohmann::json & binding, const nlohmann::json & context) {
        std::set<std::size_t> resources;
        for (const auto & value : binding) {
            if (!value.is_null()) resources.insert(value.get<std::size_t>());
        }
        return nlohmann::json(resources) == context;
    }

    std::optional<Mapping> correspondence(std::size_t target, const nlohmann::json & destination, std::size_t donor, const nlohmann::json & source) const {
        std::set<Mapping> mappings;
        for (const auto & [key, bindings] : anchors_[target]) {
            const auto found = anchors_[donor].find(key);
            if (found == anchors_[donor].end()) continue;

            for (const auto & to : bindings) {
                if (!covers(to, destination.at("context"))) continue;
                for (const auto & from : found->second) {
                    if (!covers(from, source.at("context"))) continue;
                    Mapping mapping;
                    bool compatible = true;
                    for (std::size_t index = 0; index < from.size(); ++index) {
                        if (from[index].is_null() || to[index].is_null()) {
                            compatible &= from[index].is_null() && to[index].is_null();
                            continue;
                        }
                        const auto a = from[index].get<std::size_t>();
                        const auto b = to[index].get<std::size_t>();
                        mapping.emplace(a, b);
                        compatible &= sources_[donor].kinds_.at(a) == sources_[target].kinds_.at(b);
                    }
                    if (!compatible) continue;

                    const auto & caller = source.at("caller");
                    if (caller.is_null()) compatible &= destination.at("caller").is_null();
                    else compatible &= nlohmann::json(mapping.at(caller.get<std::size_t>())) == destination.at("caller");
                    if (compatible) mappings.insert(std::move(mapping));
                }
            }
        }
        if (mappings.size() == 1) return *mappings.begin();
        return std::nullopt;
    }

    void extend(std::size_t target, nlohmann::json & selection) {
        std::set<nlohmann::json> present;
        for (const auto & choice : selection.at("choices")) present.insert(guard_signature(choice.at("guards")));
        std::map<nlohmann::json, std::map<nlohmann::json, nlohmann::json>> proposals;
        const auto key = contract(selection);

        // Only original observations qualify as evidence; borrowed choices cannot propagate again.
        for (std::size_t donor = 0; donor < sources_.size(); ++donor) {
            if (donor == target || sources_[target].domain_ != sources_[donor].domain_) continue;
            for (std::size_t variant = 0; variant < variants_[donor].size(); ++variant) {
                const auto & source = variants_[donor][variant];
                if (contract(source) != key) continue;
                const auto mapping = correspondence(target, selection, donor, source);
                if (!mapping) continue;

                for (auto choice : source.at("choices")) {
                    const auto guards = guard_signature(choice.at("guards"));
                    if (present.contains(guards)) continue;
                    choice["resources"] = sources_[donor].context_bindings(choice.at("resources"));
                    for (auto & resource : choice["resources"]) {
                        if (!resource.is_null()) resource = mapping->at(resource.get<std::size_t>());
                    }
                    auto signature = choice;
                    signature["guards"] = guards;
                    choice["origin"] = {
                        {      "rank", roles_[donor].at("environment").at("rank") },
                        { "selection",                                    variant }
                    };
                    proposals[guards].try_emplace(std::move(signature), std::move(choice));
                }
            }
        }

        for (auto & [_, alternatives] : proposals) {
            if (alternatives.size() != 1) continue;
            auto choice = std::move(alternatives.begin()->second);
            for (auto & resource : choice["resources"]) {
                if (!resource.is_null()) continue;

                // Operation-private events receive fresh identities in the destination role.
                resource = roles_[target]["execution_resources"].size();
                roles_[target]["execution_resources"].push_back("event");
            }
            selection["choices"].push_back(std::move(choice));
        }
    }

    std::span<ExecutionSource> sources_;
    nlohmann::json & roles_;
    std::vector<Anchors> anchors_;
    std::vector<nlohmann::json> variants_;
};

void ExecutionSource::share(std::span<ExecutionSource> sources, nlohmann::json & roles) { Reuse(sources, roles).publish(); }

} // namespace markov
