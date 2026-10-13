#pragma once

#include "markov/performance/service_work.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace markov {

// Explicit conditional inputs, not a fitted performance model.
class NodeDurations {
public:
    void load(const std::filesystem::path & path, const std::filesystem::path & base_manifest);
    bool contains(const std::filesystem::path & manifest) const;
    std::optional<std::int64_t> duration(const ServiceWork & work) const;
    nlohmann::json describe(const std::filesystem::path & manifest) const;

private:
    struct Source {
        std::filesystem::path path;
        std::vector<std::string> assumptions;
        std::map<SourceSpanId, std::int64_t> values;
    };

    std::map<std::filesystem::path, Source> sources_;
};

} // namespace markov
