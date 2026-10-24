#pragma once

#include <cstddef>
#include <map>

#include <nlohmann/json.hpp>

namespace markov {

// Role bindings stay independent while normalized work and layouts share one source-domain catalog.
class TemplateLibrary {
public:
    void add(const nlohmann::json & source);
    nlohmann::json document() &&;

private:
    friend class SourceTemplates;

    std::map<nlohmann::json, std::size_t> definitions_;
    std::map<nlohmann::json, std::size_t> layouts_;
    std::map<nlohmann::json, std::size_t> execution_patterns_;
    nlohmann::json roles_ = nlohmann::json::array();
};

} // namespace markov
