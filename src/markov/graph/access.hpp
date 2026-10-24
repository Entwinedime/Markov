#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

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
std::vector<Dependency> connect_accesses(std::span<const ValueAccess> values, std::span<const Operation> operations);

} // namespace markov
