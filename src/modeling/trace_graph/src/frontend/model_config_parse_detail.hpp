/**
 * @file
 * @brief Internal declarations shared by the narrow model-config parsers.
 */
#pragma once

#include "markov/trace_graph/frontend/model_config.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace markov::trace_graph::frontend::model_config_detail {

using Json = nlohmann::json;

inline constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kIoKinds{
    std::pair{              "prefetch", "storage_to_host" },
    std::pair{                  "load",  "host_to_device" },
    std::pair{  "write_device_to_host",  "device_to_host" },
    std::pair{ "write_host_to_storage", "host_to_storage" },
};

void require_known_fields(const Json & object, const std::string & context, std::initializer_list<const char *> allowed);
void require_exact_fields(const Json & object, const std::string & context, std::initializer_list<const char *> required);
[[nodiscard]] std::string lower(std::string value);
[[nodiscard]] bool is_allowed_policy(const std::string & value, std::initializer_list<const char *> allowed);
[[nodiscard]] std::string string_value(const Json & object, const std::string & key, const std::string & fallback);
[[nodiscard]] double number_value(const Json & object, const std::string & key, double fallback);
[[nodiscard]] uint64_t u64_value(const Json & object, const std::string & key, uint64_t fallback);
[[nodiscard]] bool bool_value(const Json & object, const std::string & key, bool fallback);
[[nodiscard]] std::map<std::string, std::string> string_map_value(const Json & object, const std::string & key);

[[nodiscard]] std::map<std::string, HiCacheIoServiceModelConfig> parse_hicache_service_models(const Json & io_cost);
[[nodiscard]] HiCacheIoPlanningConfig parse_hicache_io_planning(const Json & hicache);
[[nodiscard]] HiCacheIoCostConfig parse_hicache_io_cost(const Json & hicache);

} // namespace markov::trace_graph::frontend::model_config_detail
