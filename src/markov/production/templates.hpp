#pragma once

#include <span>

#include "markov/graph/graph.hpp"
#include "markov/templates/library.hpp"

namespace markov {

struct EntrySpecification {
    std::size_t scope;
    std::string action;
    nlohmann::json condition;
};

// Compile declared parameter relations and hierarchical coarse structure, not observed numeric coincidences.
class TemplateCompiler {
public:
    static TemplateLibrary compile(const CoarseGraph & source, std::span<const EntrySpecification> entries, nlohmann::json domain);
};

} // namespace markov
