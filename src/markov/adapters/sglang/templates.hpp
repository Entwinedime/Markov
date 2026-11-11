#pragma once

#include <cstddef>
#include <map>
#include <string_view>
#include <vector>

#include "markov/graph/graph.hpp"
#include "markov/production/templates.hpp"
#include "markov/templates/library.hpp"

namespace markov::sglang {

struct TemplateDeclarations {
    CoarseGraph graph;
    std::vector<EntrySpecification> entries;
};

// Native facts select execution classes; capture and control share the same template contract.
class NativeTemplates {
public:
    static TemplateDeclarations describe(const CoarseGraph & observation);
    static TemplateLibrary compile(const CoarseGraph & observation);
    static nlohmann::json condition(std::string_view action, nlohmann::json facts);
    static nlohmann::json domain(const nlohmann::json & environment);
    static nlohmann::json share_batch_transforms(std::map<std::size_t, TemplateLibrary> & libraries);
};

} // namespace markov::sglang
