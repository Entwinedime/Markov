#include "markov/graph/access.hpp"

#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov {

std::vector<std::size_t> remap(std::span<const std::size_t> identities, std::span<const std::size_t> bindings) {
    std::vector<std::size_t> result;
    result.reserve(identities.size());

    for (const auto identity : identities) {
        if (identity >= bindings.size()) throw std::out_of_range("identity binding is out of range");

        result.push_back(bindings[identity]);
    }

    return result;
}

std::vector<Dependency> connect_accesses(std::span<const ValueAccess> values, std::span<const Operation> operations) {
    struct Access {
        std::optional<std::size_t> writer;
        std::set<std::size_t> readers;
    };

    std::map<std::size_t, Access> accesses;
    std::vector<Dependency> dependencies;

    for (std::size_t identity = 0; identity < operations.size(); ++identity) {
        const auto & operation = operations[identity];
        std::set<std::pair<std::size_t, std::string>> predecessors;

        // A value edge preserves production; storage edges also preserve aliasing hazards.
        for (const auto input : operation.inputs) {
            if (input >= values.size()) throw std::invalid_argument("operation input value is out of range");

            const auto & value = values[input];
            if (value.producer) predecessors.emplace(*value.producer, "value");

            auto & access = accesses[value.storage];
            if (access.writer) predecessors.emplace(*access.writer, "read_after_write");
            access.readers.insert(identity);
        }

        // Whole-storage alias order is conservative; exact KV page effects use state ports.
        for (const auto storage : operation.writes) {
            auto & access = accesses[storage];
            if (access.writer) predecessors.emplace(*access.writer, "write_after_write");

            for (const auto reader : access.readers) {
                if (reader != identity) predecessors.emplace(reader, "write_after_read");
            }

            access.readers.clear();
            access.writer = identity;
        }

        for (const auto & [predecessor, kind] : predecessors) {
            if (predecessor >= identity) throw std::invalid_argument("operation input is not an earlier value");

            dependencies.push_back({ predecessor, identity, kind });
        }
    }

    return dependencies;
}

} // namespace markov
