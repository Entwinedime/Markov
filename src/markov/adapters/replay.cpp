#include "markov/adapters/replay.hpp"

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace markov::adapters {

PreparedReplay prepare_replay(const std::filesystem::path & path) {
    std::ifstream stream(path);
    const auto raw = nlohmann::json::parse(stream);
    for (const auto & [key, value] : raw.items())
        if (key != "manifest" && key != "node_scale" && key != "overrides" && key != "inspect_name")
            throw std::runtime_error("Unknown static replay field: " + key);

    auto manifest = std::filesystem::path(raw.at("manifest").get<std::string>());
    if (manifest.is_relative()) manifest = path.parent_path() / manifest;
    auto scaling = read_node_scale(raw.value("node_scale", nlohmann::json::array()));
    std::map<trace_graph::graph::NodeId, trace_graph::graph::Time> overrides;
    for (const auto & item : raw.value("overrides", nlohmann::json::array())) {
        if (!item.is_object() || item.size() != 2 || !item.at("node").is_number_integer() || !item.at("duration_ns").is_number_integer())
            throw std::runtime_error("A replay override requires integer node and duration_ns");
        const auto node = item.at("node").get<std::int64_t>();
        const auto duration = item.at("duration_ns").get<trace_graph::graph::Time>();
        if (node < 0 || duration < 0) throw std::runtime_error("Replay override node or nanosecond duration is outside range");
        if (!overrides.emplace(node, duration).second) throw std::runtime_error("Duplicate replay node override");
    }
    auto inspect = raw.contains("inspect_name") ? std::optional(raw.at("inspect_name").get<std::string>()) : std::nullopt;
    return { prepare_source(trace_graph::source::inspect_manifest(manifest)), std::move(scaling), std::move(overrides), std::move(inspect) };
}

} // namespace markov::adapters
