/**
 * @file
 * @brief Parser for the compact HiCache service coefficients.
 */
#include "markov/trace_graph/frontend/model_config_parse_detail.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace markov::trace_graph::frontend::model_config_detail {

namespace {

double positive_number(const Json & object, const std::string & field, const std::string & context) {
    const auto value = number_value(object, field, 0.0);
    if (!std::isfinite(value) || value <= 0.0) throw std::runtime_error(context + "." + field + " must be positive");
    return value;
}

double nonnegative_number(const Json & object, const std::string & field, const std::string & context) {
    const auto value = number_value(object, field, 0.0);
    if (!std::isfinite(value) || value < 0.0) throw std::runtime_error(context + "." + field + " must be non-negative");
    return value;
}

std::vector<HiCacheIoPageBandwidthPoint> page_bandwidth_points(const Json & object, const std::string & field, const std::string & context) {
    const auto it = object.find(field);
    if (it == object.end() || !it->is_array() || it->empty()) throw std::runtime_error(context + "." + field + " requires measured anchors");
    std::vector<HiCacheIoPageBandwidthPoint> points;
    uint64_t previous = 0;
    for (const auto & raw : *it) {
        const auto page_bytes = u64_value(raw, "page_bytes", 0);
        const auto bandwidth = positive_number(raw, "bandwidth_bytes_per_sec", context + "." + field);
        if (page_bytes <= previous) throw std::runtime_error(context + "." + field + " page anchors must increase");
        points.push_back({ .page_bytes = page_bytes,
                           .bandwidth_bytes_per_sec = bandwidth,
                           .setup_us_per_operation = nonnegative_number(raw, "setup_us_per_operation", context + "." + field) });
        previous = page_bytes;
    }
    return points;
}

std::vector<HiCacheIoNewOperationPoint> new_operation_points(const Json & object, const std::string & context) {
    const auto it = object.find("new_operation_points");
    if (it == object.end() || !it->is_array() || it->empty()) throw std::runtime_error(context + ".new_operation_points requires measured anchors");
    std::vector<HiCacheIoNewOperationPoint> points;
    uint64_t previous = 0;
    for (const auto & raw : *it) {
        const auto page_bytes = u64_value(raw, "page_bytes", 0);
        if (page_bytes <= previous) throw std::runtime_error(context + ".new_operation_points page anchors must increase");
        points.push_back({
            .page_bytes = page_bytes,
            .setup_us_per_operation = nonnegative_number(raw, "setup_us_per_operation", context + ".new_operation_points"),
            .bandwidth_bytes_per_sec = positive_number(raw, "bandwidth_bytes_per_sec", context + ".new_operation_points"),
        });
        previous = page_bytes;
    }
    return points;
}

std::vector<HiCacheIoExistingKeyBandwidthPoint> existing_key_points(const Json & object, const std::string & context) {
    const auto it = object.find("existing_key_bandwidth_points");
    if (it == object.end() || !it->is_array() || it->empty()) throw std::runtime_error(context + ".existing_key_bandwidth_points requires measured anchors");
    std::vector<HiCacheIoExistingKeyBandwidthPoint> points;
    std::pair<uint64_t, uint64_t> previous{};
    bool first = true;
    for (const auto & raw : *it) {
        const std::pair coordinate{ u64_value(raw, "page_bytes", 0), u64_value(raw, "operation_pages", 0) };
        if (coordinate.first == 0 || coordinate.second == 0 || (!first && coordinate <= previous))
            throw std::runtime_error(context + ".existing_key_bandwidth_points coordinates must increase");
        points.push_back({
            .page_bytes = coordinate.first,
            .operation_pages = coordinate.second,
            .bandwidth_bytes_per_sec = positive_number(raw, "bandwidth_bytes_per_sec", context + ".existing_key_bandwidth_points"),
        });
        previous = coordinate;
        first = false;
    }
    return points;
}

} // namespace

std::map<std::string, HiCacheIoServiceModelConfig> parse_hicache_service_models(const Json & io_cost) {
    const auto models = io_cost.find("service_models");
    if (models == io_cost.end() || !models->is_object())
        throw std::runtime_error("hicache.io_cost.service_models must be an object of available service costs");
    require_known_fields(*models, "hicache.io_cost.service_models", { "prefetch", "load", "write_device_to_host", "write_host_to_storage" });
    std::map<std::string, HiCacheIoServiceModelConfig> output;
    for (const auto & [kind_view, expected_direction_view] : kIoKinds) {
        const std::string kind(kind_view);
        const std::string expected_direction(expected_direction_view);
        const auto raw = models->find(kind);
        if (raw == models->end()) continue;
        if (!raw->is_object() || string_value(*raw, "direction", "") != expected_direction) throw std::runtime_error("Invalid service direction for " + kind);
        const std::string context = "hicache.io_cost.service_models." + kind;
        HiCacheIoServiceModelConfig model{ .direction = expected_direction };
        if (kind == "prefetch") {
            require_exact_fields(*raw, context, { "direction", "runtime_scale", "stages" });
            const auto & stages = raw->at("stages");
            require_exact_fields(stages,
                                 context + ".stages",
                                 { "before_copy_us_per_page",
                                   "before_copy_us_per_byte",
                                   "copy_publish_us_per_page",
                                   "copy_publish_us_per_byte",
                                   "return_us_per_operation",
                                   "return_us_per_page" });
            model.stages = HiCachePrefetchStagesConfig{
                nonnegative_number(stages, "before_copy_us_per_page", context),  nonnegative_number(stages, "before_copy_us_per_byte", context),
                nonnegative_number(stages, "copy_publish_us_per_page", context), nonnegative_number(stages, "copy_publish_us_per_byte", context),
                nonnegative_number(stages, "return_us_per_operation", context),  nonnegative_number(stages, "return_us_per_page", context)
            };
            model.runtime_scale = positive_number(*raw, "runtime_scale", context);
        }
        else if (kind == "load" || kind == "write_device_to_host") {
            require_exact_fields(*raw, context, { "direction", "page_bandwidth_points", "runtime_scale" });
            model.page_bandwidth_points = page_bandwidth_points(*raw, "page_bandwidth_points", context);
            model.runtime_scale = positive_number(*raw, "runtime_scale", context);
        }
        else {
            require_known_fields(*raw, context, { "direction", "new_operation_points", "existing_key_bandwidth_points", "existing_runtime_scale" });
            if (raw->contains("new_operation_points")) model.new_operation_points = new_operation_points(*raw, context);
            if (raw->contains("existing_key_bandwidth_points")) {
                model.existing_key_bandwidth_points = existing_key_points(*raw, context);
                model.existing_runtime_scale = positive_number(*raw, "existing_runtime_scale", context);
            }
            else if (raw->contains("existing_runtime_scale")) throw std::runtime_error(context + ".existing_runtime_scale requires existing-key costs");
            if (model.new_operation_points.empty() && model.existing_key_bandwidth_points.empty())
                throw std::runtime_error(context + " requires new or existing write costs");
        }
        output.emplace(kind, std::move(model));
    }
    return output;
}

} // namespace markov::trace_graph::frontend::model_config_detail
