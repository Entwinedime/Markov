#pragma once

#include <cstddef>
#include <map>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/production/execution_source.hpp"

namespace markov {

// Role bindings stay independent while normalized work and layouts share one source-domain catalog.
class TemplateLibrary {
public:
    void add(const nlohmann::json & source);
    void add_instances(const nlohmann::json & instances);
    nlohmann::json document() &&;

private:
    friend class SourceTemplates;

    std::map<nlohmann::json, std::size_t> definitions_;
    std::map<nlohmann::json, std::size_t> layouts_;
    std::map<nlohmann::json, std::size_t> execution_patterns_;
    std::vector<ExecutionSource> execution_sources_;
    nlohmann::json roles_ = nlohmann::json::array();
};

} // namespace markov
