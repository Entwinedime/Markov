/**
 * @file
 * @brief Parser for compact HiCache control and resource-lane coefficients.
 */
#include "model_config_parse_detail.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace markov::trace_graph::frontend::model_config_detail {

namespace {

std::map<std::string, HiCacheIoControlModelConfig> control_models(const Json & io_cost) {
    const auto models = io_cost.find("control_models");
    if (models == io_cost.end() || !models->is_object() || models->size() != kIoKinds.size())
        throw std::runtime_error("hicache.io_cost.control_models must contain four families");
    std::map<std::string, HiCacheIoControlModelConfig> output;
    for (const auto & [kind_view, direction] : kIoKinds) {
        (void)direction;
        const std::string kind(kind_view);
        const auto raw = models->find(kind);
        if (raw == models->end() || !raw->is_object()) throw std::runtime_error("Missing control model for " + kind);
        require_exact_fields(*raw,
                             "hicache.io_cost.control_models." + kind,
                             { "fixed_us_per_operation", "zero_payload_fixed_us_per_operation", "per_page_us" });
        HiCacheIoControlModelConfig model{
            .fixed_us_per_operation = number_value(*raw, "fixed_us_per_operation", 0.0),
            .zero_payload_fixed_us_per_operation = number_value(*raw, "zero_payload_fixed_us_per_operation", 0.0),
            .per_page_us = number_value(*raw, "per_page_us", 0.0),
        };
        if (!std::isfinite(model.fixed_us_per_operation) || model.fixed_us_per_operation < 0.0
            || !std::isfinite(model.zero_payload_fixed_us_per_operation) || model.zero_payload_fixed_us_per_operation < 0.0
            || !std::isfinite(model.per_page_us) || model.per_page_us < 0.0)
            throw std::runtime_error("Control coefficients must be finite and non-negative for " + kind);
        output.emplace(kind, model);
    }
    return output;
}

HiCacheIoResourceLanesConfig resource_lanes(const Json & io_cost) {
    const auto raw = io_cost.find("resource_lanes");
    if (raw == io_cost.end() || !raw->is_object()) throw std::runtime_error("hicache.io_cost.resource_lanes must be an object");
    require_exact_fields(*raw, "hicache.io_cost.resource_lanes", { "storage_read", "storage_write" });
    const auto read = string_value(*raw, "storage_read", "");
    const auto write = string_value(*raw, "storage_write", "");
    if ((read != "shared" && read != "scope") || (write != "shared" && write != "scope"))
        throw std::runtime_error("Storage resource lanes must be 'shared' or 'scope'");
    return {
        .shared_storage_read = read == "shared",
        .shared_storage_write = write == "shared",
    };
}

} // namespace

HiCacheIoPlanningConfig parse_hicache_io_planning(const Json & object) {
    const auto raw = object.find("io_planning");
    if (raw == object.end()) return {};
    if (!raw->is_object()) throw std::runtime_error("hicache.io_planning must be an object");
    require_exact_fields(*raw,
                         "hicache.io_planning",
                         { "device_host_bandwidth_bytes_per_sec", "host_storage_bandwidth_bytes_per_sec" });
    HiCacheIoPlanningConfig config{
        .device_host_bandwidth_bytes_per_sec = u64_value(*raw, "device_host_bandwidth_bytes_per_sec", 0),
        .host_storage_bandwidth_bytes_per_sec = u64_value(*raw, "host_storage_bandwidth_bytes_per_sec", 0),
    };
    if (config.device_host_bandwidth_bytes_per_sec == 0 || config.host_storage_bandwidth_bytes_per_sec == 0)
        throw std::runtime_error("HiCache effect planning bandwidths must be positive");
    return config;
}

HiCacheIoCostConfig parse_hicache_io_cost(const Json & object) {
    HiCacheIoCostConfig config;
    const auto raw = object.find("io_cost");
    if (raw == object.end()) return config;
    if (!raw->is_object()) throw std::runtime_error("hicache.io_cost must be an object");
    require_exact_fields(*raw,
                         "hicache.io_cost",
                         { "service_models", "control_models", "resource_lanes" });
    config.service_models = parse_hicache_service_models(*raw);
    config.control_models = control_models(*raw);
    config.resource_lanes = resource_lanes(*raw);
    return config;
}

} // namespace markov::trace_graph::frontend::model_config_detail
