#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace markov {

struct CostQuery;

// Explicit conditional inputs, not a fitted performance model.
class NodeDurations {
public:
    void load(const std::filesystem::path & path, const std::filesystem::path & base_manifest);
    bool contains(const std::filesystem::path & manifest) const;
    std::optional<std::int64_t> duration(const std::filesystem::path & manifest, const CostQuery & query) const;
    nlohmann::json describe(const std::filesystem::path & manifest) const;

private:
    struct Rule {
        nlohmann::json work;
        std::int64_t duration_ns;
    };

    struct Source {
        std::filesystem::path path;
        std::vector<std::string> assumptions;
        std::vector<Rule> values;
    };

    std::map<std::filesystem::path, Source> sources_;
};

} // namespace markov
