#pragma once

#include <span>
#include <vector>

#include "markov/graph/graph.hpp"
#include "markov/templates/library.hpp"

namespace markov {

struct EntrySpecification {
    std::size_t scope;
    std::string action;
    nlohmann::json condition;
};

// Source event identities locate measurements; the definition and its interval locate reusable work.
struct TemplateWorkEvidence {
    std::size_t definition;
    std::size_t interval;
    std::size_t start;
    std::size_t finish;
};

// Compile declared parameter relations and hierarchical coarse structure, not observed numeric coincidences.
class TemplateCompiler {
public:
    static TemplateLibrary compile(const CoarseGraph & source, std::span<const EntrySpecification> entries, nlohmann::json domain,
                                   std::vector<TemplateWorkEvidence> * evidence = nullptr);
};

} // namespace markov
