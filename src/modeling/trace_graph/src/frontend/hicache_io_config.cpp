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
        const auto coefficient = [&](const std::string & field) {
            const auto value = number_value(*raw, field, 0.0);
            if (!std::isfinite(value) || value < 0.0) throw std::runtime_error("Invalid control coefficient " + kind + "." + field);
            return value;
        };
        HiCacheIoControlModelConfig model{ .fixed_us_per_operation = coefficient("fixed_us_per_operation") };
        const auto path = "hicache.io_cost.control_models." + kind;
        require_exact_fields(*raw, path, { "fixed_us_per_operation" });
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

HiCacheIoCostConfig parse_hicache_io_cost(const Json & object) {
    HiCacheIoCostConfig config;
    const auto raw = object.find("io_cost");
    if (raw == object.end()) return config;
    if (!raw->is_object()) throw std::runtime_error("hicache.io_cost must be an object");
    require_exact_fields(*raw, "hicache.io_cost", { "storage_batch_pages", "service_models", "control_models", "resource_lanes" });
    config.storage_batch_pages = u64_value(*raw, "storage_batch_pages", 0);
    if (config.storage_batch_pages == 0) throw std::runtime_error("HiCache storage batch pages must be positive");
    config.service_models = parse_hicache_service_models(*raw);
    config.control_models = control_models(*raw);
    config.resource_lanes = resource_lanes(*raw);
    return config;
}

} // namespace markov::trace_graph::frontend::model_config_detail
