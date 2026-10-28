#include "markov/adapters/sglang/cache.hpp"

#include "markov/trace_graph/planning/cache_program.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>

namespace markov::adapters::sglang {

CacheSettings read_cache_settings(const nlohmann::json & raw) {
    const std::set<std::string> fields{ "device_pages", "host_pages", "write_policy", "io_backend", "host_layout" };
    if (!raw.is_object()) throw std::runtime_error("Cache settings must be an object");
    for (const auto & [key, value] : raw.items()) {
        if (!fields.contains(key)) throw std::runtime_error("Unknown cache setting: " + key);
    }
    for (const auto * name : { "device_pages", "host_pages" }) {
        if (!raw.at(name).is_number_integer() || raw.at(name).get<std::int64_t>() < 1)
            throw std::runtime_error("Cache capacity must be a positive page count: " + std::string(name));
    }
    if (raw.at("io_backend") != "kernel_ascend" || raw.at("host_layout") != "page_first_direct")
        throw std::runtime_error("Cache execution currently supports only kernel_ascend/page_first_direct");
    const auto policy = raw.at("write_policy").get<std::string>();
    std::optional<std::size_t> hits;
    if (policy == "write_through") hits = 1;
    else if (policy == "write_through_selective") hits = 2;
    else if (policy != "write_back") throw std::runtime_error("Unsupported cache write policy: " + policy);
    return { raw.at("device_pages").get<std::size_t>(), raw.at("host_pages").get<std::size_t>(), hits };
}

trace_graph::planning::CacheBackend cache_backend(const trace_graph::source::SourceContext & source) {
    using namespace trace_graph;
    std::optional<planning::AttentionGeometry> geometry;
    std::map<source::Index, std::set<source::Index>> layers;
    std::set<source::Index> participants;
    for (const auto & call : source.observations().work_observations) {
        const auto & recipe = source.observations().work_descriptions[call.description].recipe;
        for (const auto & part : recipe) {
            const auto * attention = std::get_if<planning::AttentionGeometry>(&part.operation.geometry);
            if (!attention) continue;
            const auto writes = std::ranges::count_if(recipe, [](const planning::ComputePart & item) {
                return std::holds_alternative<planning::CacheWriteGeometry>(item.operation.geometry);
            });
            if (writes != 2) throw std::runtime_error("Cache geometry needs observed K/V writes, not attention alone");
            if (!call.layer
                || (geometry
                    && (geometry->kv_heads != attention->kv_heads || geometry->query_key_dim != attention->query_key_dim
                        || geometry->value_dim != attention->value_dim)))
                throw std::runtime_error("Cache layout is not uniform in the source");
            geometry = *attention;
            layers[call.frame].insert(*call.layer);
            participants.insert(source.observations().compute_frames[call.frame].record.file);
        }
    }
    if (!geometry || layers.size() != source.observations().compute_frames.size())
        throw std::runtime_error("Cache layout needs a complete supported attention recipe in each source frame");
    const auto & reference = layers.begin()->second;
    if (*reference.begin() != 0 || *reference.rbegin() + 1 != reference.size()) throw std::runtime_error("Observed cache layers are not contiguous");
    for (const auto & [frame, current] : layers) {
        if (current != reference) throw std::runtime_error("Observed cache layer sets differ between source frames");
    }
    // The supported attention recipe is NPUMHA/FIA with BF16 K/V, validated
    // when its native cache-write descriptor is interpreted.
    const auto bytes_per_token = reference.size() * geometry->kv_heads * (geometry->query_key_dim + geometry->value_dim) * 2;
    return {
        reference.size(),
        static_cast<std::size_t>(bytes_per_token),
        participants.size(),
        10,
        {   "cache.load", "kernel_ascend.page_first_direct", "device_service" },
        { "cache.backup", "kernel_ascend.page_first_direct", "device_service" }
    };
}

} // namespace markov::adapters::sglang
