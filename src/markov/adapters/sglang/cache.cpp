#include "markov/adapters/sglang/cache.hpp"
#include "markov/adapters/sglang/cache_indices.hpp"

#include "markov/trace_graph/planning/cache_program.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>

namespace markov::adapters::sglang {

std::size_t host_capacity_pages(std::size_t device_tokens, std::size_t page_tokens, double ratio) {
    const double tokens = static_cast<double>(device_tokens) * ratio;
    if (!std::isfinite(ratio) || ratio <= 0 || !std::isfinite(tokens)
        || tokens >= static_cast<long double>(std::numeric_limits<std::size_t>::max() - page_tokens))
        throw std::runtime_error("HiCache ratio exceeds the supported host-token range");

    const auto pages = static_cast<std::size_t>(tokens) / page_tokens + 1;
    if (pages <= device_tokens / page_tokens) throw std::runtime_error("SGLang requires a larger host pool than device pool");
    return pages;
}

std::size_t minimum_device_tokens(std::size_t prompt_tokens, std::size_t output_tokens, std::size_t page_tokens) {
    const auto paged = ((prompt_tokens + page_tokens - 1) / page_tokens) * page_tokens;
    const auto limit = std::numeric_limits<std::size_t>::max();
    if (paged > limit - page_tokens || output_tokens >= limit - paged - page_tokens)
        throw std::overflow_error("SGLang admission reservation exceeds the token-count range");
    return paged + output_tokens + page_tokens + 1;
}

namespace {

trace_graph::planning::StorageConfig read_storage(const nlohmann::json & raw, std::size_t device_pages, std::size_t host_pages) {
    using namespace trace_graph::planning;
    const std::set<std::string> fields{ "backend", "initial_state", "capacity_pages", "prefetch_stop_policy", "prefetch_threshold_tokens", "prefetch_timeout" };
    if (!raw.is_object()) throw std::runtime_error("Storage settings must be an object");
    for (const auto & [key, value] : raw.items()) {
        if (!fields.contains(key)) throw std::runtime_error("Unknown storage setting: " + key);
    }
    if (raw.at("backend") != "file" || raw.at("initial_state") != "empty")
        throw std::runtime_error("Storage execution currently requires file storage with declared empty initial contents");
    std::optional<std::size_t> capacity;
    if (raw.contains("capacity_pages") && !raw.at("capacity_pages").is_null()) {
        if (!raw.at("capacity_pages").is_number_integer() || raw.at("capacity_pages").get<std::int64_t>() < 1)
            throw std::runtime_error("Storage capacity must be a positive page count or null");
        capacity = raw.at("capacity_pages").get<std::size_t>();
    }
    const auto policy = raw.at("prefetch_stop_policy").get<std::string>();
    PrefetchPolicy stop;
    if (policy == "wait_complete") stop = PrefetchPolicy::Complete;
    else if (policy == "best_effort") stop = PrefetchPolicy::BestEffort;
    else if (policy == "timeout") stop = PrefetchPolicy::Timeout;
    else throw std::runtime_error("Unsupported prefetch policy: " + policy);
    const auto threshold = raw.value("prefetch_threshold_tokens", nlohmann::json(256));
    if (!threshold.is_number_integer() || threshold.get<std::int64_t>() < 1) throw std::runtime_error("Prefetch threshold must be positive tokens");
    auto timeout = raw.value("prefetch_timeout", nlohmann::json::object());
    for (const auto & [key, value] : timeout.items()) {
        if (key != "base_sec" && key != "per_ki_token_sec" && key != "max_sec") throw std::runtime_error("Unknown prefetch timeout field: " + key);
    }
    const auto ns = [](double seconds) {
        const auto value = static_cast<long double>(seconds) * 1e9L;
        if (!std::isfinite(value) || value < 0 || value >= std::numeric_limits<trace_graph::graph::Time>::max())
            throw std::runtime_error("Prefetch timeout must be nonnegative finite seconds within the clock range");
        return static_cast<trace_graph::graph::Time>(std::round(value));
    };
    return { capacity,
             stop,
             threshold.get<std::size_t>(),
             host_pages > device_pages ? 0.8 * (host_pages - device_pages) : 0,
             ns(timeout.value("base_sec", 2.0)),
             ns(timeout.value("per_ki_token_sec", 0.1)),
             ns(timeout.value("max_sec", 30.0)) };
}

} // namespace

std::optional<trace_graph::planning::StorageConfig> configured_storage(const nlohmann::json & configuration, std::size_t device_pages, std::size_t host_pages) {
    using Json = nlohmann::json;
    if (!configuration.contains("hicache_storage_backend") || configuration.at("hicache_storage_backend").is_null()) return std::nullopt;
    if (configuration.at("hicache_storage_backend") != "file" || !configuration.at("enable_hierarchical_cache").get<bool>())
        throw std::runtime_error("Storage prediction requires file storage with HiCache enabled");

    Json extra = Json::object();
    if (configuration.contains("hicache_storage_backend_extra_config") && !configuration.at("hicache_storage_backend_extra_config").is_null()) {
        const auto value = configuration.at("hicache_storage_backend_extra_config").get<std::string>();
        if (value.starts_with("@")) throw std::runtime_error("Storage prediction requires inline extra configuration, not an external target file");
        extra = Json::parse(value);
    }
    const std::set<std::string> fields{ "prefetch_threshold", "prefetch_timeout_base", "prefetch_timeout_per_ki_token", "prefetch_timeout_max" };
    for (const auto & [key, value] : extra.items()) {
        if (!fields.contains(key)) throw std::runtime_error("Storage prediction does not yet support extra setting: " + key);
    }
    return read_storage(
        {
            { "backend", "file" },
            { "initial_state", "empty" },
            { "prefetch_stop_policy", configuration.value("hicache_storage_prefetch_policy", "timeout") },
            { "prefetch_threshold_tokens", extra.value("prefetch_threshold", Json(256)) },
            { "prefetch_timeout",
             { { "base_sec", extra.value("prefetch_timeout_base", 2.0) },
                { "per_ki_token_sec", extra.value("prefetch_timeout_per_ki_token", 0.1) },
                { "max_sec", extra.value("prefetch_timeout_max", 30.0) } } }
    },
        device_pages,
        host_pages);
}

CacheSettings read_cache_settings(const nlohmann::json & raw) {
    const std::set<std::string> fields{ "device_pages", "host_pages", "write_policy", "io_backend", "host_layout", "storage" };
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
    CacheSettings settings{ raw.at("device_pages").get<std::size_t>(), raw.at("host_pages").get<std::size_t>(), hits, std::nullopt };
    if (raw.contains("storage")) settings.storage = read_storage(raw.at("storage"), settings.device_pages, settings.host_pages);
    return settings;
}

trace_graph::planning::CacheBackend cache_backend(const trace_graph::source::SourceContext & source) {
    using namespace trace_graph;
    for (const auto & control : source.observations().host_controls) {
        if (control.work.kind.operation == "cache.update_prefix" && control.work.features.at("chunked") != 0)
            throw std::runtime_error("Cache execution currently requires unchunked Prefill");
        if (control.work.kind.operation == "cache.commit_prefix" && control.work.features.at("insert_requested") == 0)
            throw std::runtime_error("Cache execution does not yet support disabling finished-prefix insertion");
        if (control.work.kind.operation == "allocator.finish_release_group" && control.work.features.at("need_sort") != 0)
            throw std::runtime_error("Cache execution currently requires an unsorted free-page pool");
    }
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
        { "cache.load", "kernel_ascend.page_first_direct", "device_service" },
        { "cache.backup", "kernel_ascend.page_first_direct", "device_service" },
        { "cache.backup.caller", "sglang.HiCacheController.write:kernel_ascend.page_first_direct", "caller_blocking_interval" },
        { "cache.backup.completion", "sglang.HiCacheController.write:kernel_ascend.page_first_direct", "completion_observation_upper_interval" },
        { "cache.load.prepare", "sglang.HiRadixCache.load_back:kernel_ascend.page_first_direct", "caller_blocking_interval" },
        { "cache.load.caller", "sglang.HiCacheController.start_loading:kernel_ascend.page_first_direct", "caller_blocking_interval" },
        { "cache.load.completion", "sglang.HiCacheController.start_loading:kernel_ascend.page_first_direct", "completion_observation_upper_interval" },
        cache_backup(),
        { 128,
          { "cache.prefetch.allocate", "file.generic_page_get", "storage_worker_service" },
          { "cache.prefetch.read_page", "file.generic_page_get", "storage_worker_service" },
          { "cache.prefetch.page_publish", "file.generic_page_get", "storage_worker_service" },
          { "cache.prefetch.return", "file.generic_page_get", "storage_worker_service" },
          { "cache.storage_write.gather", "file.materialize_then_batch_set", "storage_worker_service" },
          { "cache.storage_write.batch_prepare", "file.materialize_then_batch_set", "storage_worker_service" },
          { "cache.storage_write.new_page", "file.materialize_then_batch_set", "storage_worker_service" },
          { "cache.storage_write.existing_page", "file.materialize_then_batch_set", "storage_worker_service" } }
    };
}

} // namespace markov::adapters::sglang
