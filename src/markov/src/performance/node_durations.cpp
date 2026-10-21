#include "markov/performance/node_durations.hpp"
#include "markov/contracts/json_input.hpp"
#include "markov/performance/cost_query.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace markov {
namespace {

// Objects select named work features; arrays and scalar values match exactly.
// This is an explicit conditional domain, never an implicit default cost.
bool matches(const nlohmann::json & domain, const nlohmann::json & work) {
    if (!domain.is_object()) return domain == work;
    if (!work.is_object()) return false;

    for (const auto & [name, value] : domain.items()) {
        const auto field = work.find(name);
        if (field == work.end() || !matches(value, *field)) return false;
    }
    return true;
}

} // namespace

void NodeDurations::load(const std::filesystem::path & path, const std::filesystem::path & base_manifest) {
    const auto input = markov::input::read_json(path);
    const auto source = path.parent_path() / input.at("base_manifest").get<std::string>();
    if (std::filesystem::weakly_canonical(source) != std::filesystem::weakly_canonical(base_manifest)) {
        throw std::invalid_argument("node durations must refer to the selected base manifest");
    }
    if (input.at("unit") != "ns") throw std::invalid_argument("node duration unit must be ns");

    Source loaded{ path, input.at("assumptions").get<std::vector<std::string>>(), {} };
    if (loaded.assumptions.empty()) throw std::invalid_argument("given node durations require explicit assumptions");

    for (const auto & record : input.at("values")) {
        auto domain = record.at("work");
        if (domain.at("kind").get<std::string>().empty()) throw std::invalid_argument("given duration requires a work kind");
        const auto duration = markov::input::integer(record.at("duration_ns"), 0, std::numeric_limits<std::int64_t>::max());
        loaded.values.push_back({ std::move(domain), duration });
    }

    // Publish only a fully validated source. A bad file cannot expose partial
    // values or apply another asset's conditional assumptions.
    sources_.emplace(std::filesystem::absolute(base_manifest).lexically_normal(), std::move(loaded));
}

bool NodeDurations::contains(const std::filesystem::path & manifest) const { return sources_.contains(manifest); }

std::optional<std::int64_t> NodeDurations::duration(const std::filesystem::path & manifest, const CostQuery & query) const {
    const auto asset = sources_.find(manifest);
    if (asset == sources_.end()) return std::nullopt;

    const auto work = query.describe();
    // Declaration order is explicit: a narrower domain can precede a broader
    // structural assumption without a second matching or specificity system.
    for (const auto & rule : asset->second.values)
        if (matches(rule.work, work)) return rule.duration_ns;

    return std::nullopt;
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
