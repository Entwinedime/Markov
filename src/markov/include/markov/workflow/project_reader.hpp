#pragma once

#include "markov/workflow/project.hpp"

#include <filesystem>
#include <span>

namespace markov {

class CollectionAdapter;

class ProjectReader {
public:
    explicit ProjectReader(std::span<const CollectionAdapter * const> adapters) : adapters_(adapters) {}
    Project read(const std::filesystem::path & path) const;

private:
    std::span<const CollectionAdapter * const> adapters_; // Entry-point owned through input parsing.
};

} // namespace markov
