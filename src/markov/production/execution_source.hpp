#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace markov {

struct ExecutionPrototype {
    nlohmann::json pattern;
    std::vector<std::size_t> resources;
};

// Capture-local identities become reusable execution patterns with explicit resource bindings.
class ExecutionSource {
public:
    ExecutionSource(std::map<nlohmann::json, std::size_t> & patterns, nlohmann::json domain);

    static bool equivalent(const nlohmann::json & left, const nlohmann::json & right);
    nlohmann::json observe(const nlohmann::json & operation, std::size_t identity);
    void bind(nlohmann::json & frame, std::size_t definition);
    nlohmann::json finalize(nlohmann::json & entries) const;
    static void share(std::span<ExecutionSource> sources, nlohmann::json & roles);

    const std::vector<std::string> & resources() const;
    std::size_t resource(const std::string & kind, std::int64_t identity);

private:
    class Builder;
    class Reuse;

    ExecutionPrototype compile(const nlohmann::json & observation);
    std::pair<nlohmann::json, std::vector<std::size_t>> selections() const;
    static void publish(nlohmann::json & frame, std::span<const std::size_t> groups);
    static nlohmann::json guard_signature(nlohmann::json guards);
    nlohmann::json context_bindings(const nlohmann::json & resources) const;
    std::map<nlohmann::json, std::set<nlohmann::json>> anchors() const;

    // The library owns patterns across sources; candidate bindings and resources belong to this source.
    std::map<nlohmann::json, std::size_t> & patterns_;
    nlohmann::json domain_;
    std::map<nlohmann::json, std::set<nlohmann::json>> anchors_;
    std::vector<nlohmann::json> candidates_;
    std::set<std::size_t> external_events_;
    std::map<std::pair<std::string, std::int64_t>, std::size_t> resources_;
    std::vector<std::string> kinds_;
};

} // namespace markov
