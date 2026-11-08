#pragma once

#include <string_view>

#include "markov/graph/graph.hpp"
#include "markov/templates/library.hpp"

namespace markov::sglang {

// Native facts select execution classes; capture and control share the same template contract.
class NativeTemplates {
public:
    static TemplateLibrary compile(const CoarseGraph & observation);
    static nlohmann::json condition(std::string_view action, nlohmann::json facts);
    static nlohmann::json domain(const nlohmann::json & environment);
};

} // namespace markov::sglang
