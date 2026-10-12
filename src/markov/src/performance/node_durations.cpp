#include "markov/performance/node_durations.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace markov {

void NodeDurations::load(const std::filesystem::path & path, const std::filesystem::path & base_manifest) {
    std::ifstream stream(path);
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    const auto input = nlohmann::json::parse(stream);
    const auto source = path.parent_path() / input.at("base_manifest").get<std::string>();
    if (std::filesystem::weakly_canonical(source) != std::filesystem::weakly_canonical(base_manifest)) {
        throw std::invalid_argument("node durations must refer to the selected base manifest");
    }
    if (input.at("unit") != "ns") throw std::invalid_argument("node duration unit must be ns");

    Source loaded{ path, input.at("assumptions").get<std::vector<std::string>>(), {} };
    if (loaded.assumptions.empty()) throw std::invalid_argument("given node durations require explicit assumptions");

    for (const auto & record : input.at("values")) {
        if (!record.at("source_span").is_number_unsigned()) throw std::invalid_argument("source span must be a nonnegative integer");

        const auto & value = record.at("duration_ns");
        if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > std::numeric_limits<std::int64_t>::max())) {
            throw std::invalid_argument("node duration must be an integer number of nanoseconds");
        }

        const auto duration = value.get<std::int64_t>();
        if (duration < 0) throw std::invalid_argument("node duration must be nonnegative");
        if (!loaded.values.emplace(record.at("source_span").get<SourceSpanId>(), duration).second) {
            throw std::invalid_argument("duplicate source span in node durations");
        }
    }

    // Publish only a fully validated source. A bad file cannot expose partial
    // values or contaminate another asset with colliding local span numbers.
    sources_.emplace(std::filesystem::absolute(base_manifest).lexically_normal(), std::move(loaded));
}

bool NodeDurations::contains(const std::filesystem::path & manifest) const { return sources_.contains(manifest); }

std::optional<std::int64_t> NodeDurations::duration(const std::filesystem::path & manifest, SourceSpanId source) const {
    const auto asset = sources_.find(manifest);
    if (asset == sources_.end()) return std::nullopt;
    const auto found = asset->second.values.find(source);
    if (found == asset->second.values.end()) return std::nullopt;
    return found->second;
}

nlohmann::json NodeDurations::describe(const std::filesystem::path & manifest) const {
    const auto & source = sources_.at(manifest);
    return {
        {        "path",   source.path.string() },
        {        "unit",                   "ns" },
        { "assumptions",     source.assumptions },
        {     "entries",   source.values.size() },
        {        "kind", "given_node_durations" }
    };
}

} // namespace markov
