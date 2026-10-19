#pragma once

#include <nlohmann/json_fwd.hpp>

namespace markov {

struct TemplateSource;
class SourceGraph;

// Translate native workspace identities through already-associated queue operations.
// Compile qualified native demand rules separately from source identity binding.
class AscendWorkspaceBindings {
public:
    void bind(TemplateSource & source, const nlohmann::json & facts, const SourceGraph & graph) const;
    void bind_demands(TemplateSource & source, const nlohmann::json & facts, const nlohmann::json & environment) const;
};

} // namespace markov
