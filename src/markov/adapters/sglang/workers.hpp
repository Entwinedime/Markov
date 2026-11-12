#pragma once

#include <cstdint>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "markov/templates/library.hpp"

namespace markov::sglang {

// Source-authorized relocation changes bindings, never the shared template definitions.
class WorkerBindings {
public:
    WorkerBindings(const TemplateLibrary & library, const nlohmann::json & environment);
    void bind(TemplateFrame & frame, const std::string & instance, const nlohmann::json & work) const;

private:
    void bind_worker(TemplateFrame & frame, const std::string & instance) const;
    void bind_pool(const std::string & name, const nlohmann::json & source, const nlohmann::json & target);

    const TemplateLibrary & library_;
    const nlohmann::json & source_;
    const nlohmann::json & target_;
    std::map<std::string, std::string> sites_;
    std::map<std::string, std::map<std::int64_t, std::int64_t>> layers_;
};

} // namespace markov::sglang
