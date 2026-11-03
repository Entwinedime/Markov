#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace markov {

struct ValuePorts {
    std::map<std::string, std::size_t> inputs;
    std::map<std::string, std::size_t> outputs;
};

// Value production and shared-storage hazards are separate dependency sources.
struct ValueAccess {
    std::size_t storage;
    std::optional<std::size_t> producer;
};

struct Operation {
    std::string name;
    std::size_t region;

    std::vector<std::size_t> inputs;
    std::vector<std::size_t> outputs;
    std::vector<std::size_t> writes;
};

struct Dependency {
    std::size_t source;
    std::size_t target;
    std::string kind;
};

std::vector<std::size_t> remap(std::span<const std::size_t> identities, std::span<const std::size_t> bindings);

// Expansion has a dense binding; compaction retains a sparse subset of values.
template <typename Bindings> void remap_arguments(nlohmann::json & arguments, const Bindings & values) {
    if (arguments.is_object() && arguments.size() == 1 && arguments.contains("value")) {
        arguments["value"] = values.at(arguments.at("value").get<std::size_t>());
        return;
    }

    if (arguments.is_structured()) {
        for (auto & item : arguments) remap_arguments(item, values);
    }
}

// All operation metadata follows the same value lifetime during expansion and compaction.
template <typename Bindings> void remap_work(nlohmann::json & work, const Bindings & values) {
    for (const auto * key : { "arguments", "keywords", "communication" }) {
        if (work.contains(key)) remap_arguments(work[key], values);
    }
    for (const auto * key : { "opaque_inputs", "opaque_outputs" }) {
        if (!work.contains(key)) continue;
        for (auto & value : work[key]) value = values.at(value.template get<std::size_t>());
    }
}

std::vector<Dependency> connect_accesses(std::span<const ValueAccess> values, std::span<const Operation> operations);

} // namespace markov
