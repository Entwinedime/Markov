#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
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
    ExecutionPrototype compile(const nlohmann::json & observation);

    const std::vector<std::string> & resources() const;
    std::size_t resource(const std::string & kind, std::int64_t identity);

private:
    class Builder;

    std::map<std::pair<std::string, std::int64_t>, std::size_t> resources_;
    std::vector<std::string> kinds_;
};

} // namespace markov
