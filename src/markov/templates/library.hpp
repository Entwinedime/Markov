#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/contracts/input_group.hpp"
#include "markov/production/execution_source.hpp"

namespace markov {

// Compile bottom-up so parent layouts reuse already-normalized child definitions.
class TemplateLibrary {
public:
    explicit TemplateLibrary(const nlohmann::json & source);
    nlohmann::json document() const;

private:
    struct Region {
        std::vector<std::size_t> children;
        std::vector<std::size_t> operations;

        std::vector<std::size_t> inputs;
        std::vector<std::size_t> outputs;
        std::vector<std::size_t> resources;

        std::size_t layout = 0;
        nlohmann::json frame;

        std::map<std::size_t, nlohmann::json> declarations;
        GroupValues input_groups;
        std::map<std::size_t, std::size_t> call_indices;
    };

    struct Item {
        std::size_t position;
        bool operation;
        std::size_t identity;
    };

    struct Repetition {
        std::string kind;
        nlohmann::json count;
        std::vector<std::size_t> members;
        std::vector<std::size_t> producers;
    };

    void index();
    void compile(std::size_t region);
    std::pair<nlohmann::json, std::vector<std::size_t>> execution_selections() const;
    static void publish_selections(nlohmann::json & frame, std::span<const std::size_t> groups);
    std::optional<std::size_t> host_resource(std::size_t region, const std::vector<Item> & items);
    std::map<std::size_t, std::map<std::string, Repetition>> repetitions(const std::vector<Item> & items) const;

    bool contains(std::size_t region, std::size_t operation) const;
    bool contains_region(std::size_t region, std::size_t child) const;
    void use(std::size_t region, std::size_t value);

    // The producer owns this immutable source throughout library compilation.
    const nlohmann::json & source_;
    ExecutionSource execution_;

    std::set<std::size_t> external_resources_;
    std::map<std::size_t, const nlohmann::json *> values_;
    std::map<std::string, const nlohmann::json *> modules_;
    std::vector<Region> regions_;
    std::vector<std::size_t> roots_;

    std::map<nlohmann::json, std::size_t> definitions_;
    std::map<nlohmann::json, std::size_t> layouts_;
    std::map<nlohmann::json, std::size_t> execution_patterns_;
    std::vector<nlohmann::json> execution_candidates_;
    std::set<std::size_t> external_events_;
};

} // namespace markov
