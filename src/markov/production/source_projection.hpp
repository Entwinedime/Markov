#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/graph/source_graph.hpp"

namespace markov {

// Public native interfaces retain external effects while hiding their private tensor implementation.
class SourceProjection {
public:
    explicit SourceProjection(const SourceGraph & graph);
    nlohmann::json document(const nlohmann::json * physical = nullptr) const;

private:
    struct ProjectedOperation {
        Operation access;
        std::optional<std::size_t> source;

        std::size_t position;
        nlohmann::json properties;
    };

    void select();
    void bind();

    std::optional<std::size_t> owner(const nlohmann::json & record) const;

    struct PhysicalFacts;
    PhysicalFacts physical_facts(const nlohmann::json * physical) const;
    struct RuntimeFacts;
    RuntimeFacts runtime_facts(const nlohmann::json * physical) const;

    // This view is materialized while its immutable recording graph is alive.
    const SourceGraph & graph_;
    const nlohmann::json & source_;

    std::vector<ProjectedOperation> operations_;
    std::vector<std::optional<std::size_t>> interfaces_;
    std::vector<std::size_t> owners_;
    std::map<std::size_t, std::size_t> native_;
    std::vector<ValueAccess> values_;
    std::vector<std::size_t> external_resources_;
};

} // namespace markov
