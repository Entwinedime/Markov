/**
 * @file
 * @brief One service formula shared by asynchronous state and DAG assembly.
 */
#include "markov/trace_graph/modules/hicache/service_model.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <ranges>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache {

namespace {

double log_curve(const std::vector<std::pair<double, double>> & points, double coordinate) {
    if (points.empty() || coordinate <= 0.0) return 0.0;
    if (coordinate <= points.front().first) return points.front().second;
    if (coordinate >= points.back().first) return points.back().second;
    for (size_t index = 1; index < points.size(); ++index) {
        const auto & left = points[index - 1];
        const auto & right = points[index];
        if (coordinate > right.first) continue;
        const auto position = (std::log(coordinate) - std::log(left.first)) / (std::log(right.first) - std::log(left.first));
        return std::exp(std::log(left.second) + position * (std::log(right.second) - std::log(left.second)));
    }
    return 0.0;
}

double linear_value_on_log_coordinate(const std::vector<std::pair<double, double>> & points, double coordinate) {
    if (points.empty() || coordinate <= 0.0) return 0.0;
    if (coordinate <= points.front().first) return points.front().second;
    if (coordinate >= points.back().first) return points.back().second;
    for (size_t index = 1; index < points.size(); ++index) {
        const auto & left = points[index - 1];
        const auto & right = points[index];
        if (coordinate > right.first) continue;
        const auto position = (std::log(coordinate) - std::log(left.first)) / (std::log(right.first) - std::log(left.first));
        return left.second + position * (right.second - left.second);
    }
    return 0.0;
}

double interpolated_page_bandwidth(const std::vector<frontend::HiCacheIoPageBandwidthPoint> & points, double page_bytes) {
    std::vector<std::pair<double, double>> curve;
    curve.reserve(points.size());
    for (const auto & point : points) curve.emplace_back(static_cast<double>(point.page_bytes), point.bandwidth_bytes_per_sec);
    return log_curve(curve, page_bytes);
}

double interpolated_dma_setup(const std::vector<frontend::HiCacheIoPageBandwidthPoint> & points, double page_bytes) {
    std::vector<std::pair<double, double>> curve;
    for (const auto & point : points) curve.emplace_back(static_cast<double>(point.page_bytes), point.setup_us_per_operation);
    return linear_value_on_log_coordinate(curve, page_bytes);
}

std::pair<double, double> interpolated_new_write(const frontend::HiCacheIoServiceModelConfig & model, double page_bytes) {
    std::vector<std::pair<double, double>> setup;
    std::vector<std::pair<double, double>> bandwidth;
    setup.reserve(model.new_operation_points.size());
    bandwidth.reserve(model.new_operation_points.size());
    for (const auto & point : model.new_operation_points) {
        setup.emplace_back(static_cast<double>(point.page_bytes), point.setup_us_per_operation);
        bandwidth.emplace_back(static_cast<double>(point.page_bytes), point.bandwidth_bytes_per_sec);
    }
    return {
        linear_value_on_log_coordinate(setup, page_bytes),
        log_curve(bandwidth, page_bytes),
    };
}

double interpolated_existing_key_bandwidth(const frontend::HiCacheIoServiceModelConfig & model, double page_bytes, double operation_pages) {
    std::map<uint64_t, std::vector<std::pair<double, double>>> by_page;
    for (const auto & point : model.existing_key_bandwidth_points)
        by_page[point.page_bytes].emplace_back(static_cast<double>(point.operation_pages), point.bandwidth_bytes_per_sec);
    std::vector<std::pair<double, double>> page_curve;
    page_curve.reserve(by_page.size());
    for (auto & [calibrated_page, operation_curve] : by_page) {
        std::ranges::sort(operation_curve, {}, &std::pair<double, double>::first);
        page_curve.emplace_back(static_cast<double>(calibrated_page), log_curve(operation_curve, operation_pages));
    }
    return log_curve(page_curve, page_bytes);
}

} // namespace

std::optional<HiCacheServiceCost> hicache_service_cost(const frontend::HiCacheIoServiceModelConfig & model,
                                                      uint64_t page_bytes, uint64_t pages, uint64_t calls,
                                                      uint64_t existing_pages) {
    HiCacheServiceCost cost;
    if (pages == 0) return cost;
    if (page_bytes == 0 || calls == 0 || existing_pages > pages) return std::nullopt;
    double duration = 0.0;
    if (model.direction == "host_to_storage") {
        if (existing_pages > 0) {
            const auto bandwidth = interpolated_existing_key_bandwidth(model, page_bytes, pages);
            if (bandwidth <= 0.0) return std::nullopt;
            cost.transfer_us = static_cast<double>(existing_pages) * page_bytes * 1'000'000.0 / bandwidth;
            cost.existing_us = cost.transfer_us * model.existing_runtime_scale;
        }
        if (pages > existing_pages) {
            const auto [setup, bandwidth] = interpolated_new_write(model, page_bytes);
            if (bandwidth <= 0.0) return std::nullopt;
            cost.setup_us = setup * static_cast<double>(calls);
            const auto transfer = static_cast<double>(pages - existing_pages) * page_bytes * 1'000'000.0 / bandwidth;
            cost.transfer_us += transfer;
            cost.new_us = cost.setup_us + transfer;
        }
        duration = cost.existing_us + cost.new_us;
    }
    else {
        const auto bandwidth = model.page_bandwidth_points.empty() ? model.bandwidth_bytes_per_sec
                               : interpolated_page_bandwidth(model.page_bandwidth_points, page_bytes);
        if (bandwidth <= 0.0) return std::nullopt;
        cost.bandwidth_bytes_per_sec = bandwidth / model.runtime_scale;
        const auto setup = model.page_bandwidth_points.empty() ? model.setup_us_per_operation
                           : interpolated_dma_setup(model.page_bandwidth_points, page_bytes);
        cost.setup_us = setup * static_cast<double>(calls) + model.setup_us_per_page * static_cast<double>(pages);
        cost.transfer_us = static_cast<double>(pages) * page_bytes * 1'000'000.0 / bandwidth;
        duration = (cost.setup_us + cost.transfer_us) * model.runtime_scale;
    }
    if (!std::isfinite(duration) || duration < 0.0 || duration >= static_cast<double>(std::numeric_limits<uint64_t>::max())) return std::nullopt;
    cost.duration_us = static_cast<uint64_t>(std::ceil(duration));
    return cost;
}

std::string hicache_resource_lane(const frontend::HiCacheIoCostConfig & model, const std::string & kind, const std::string & scope) {
    const auto local = kind == "prefetch" ? "host_storage_read_lane"
                       : kind == "write_host_to_storage" ? "host_storage_write_lane"
                       : kind == "load" ? "host_to_device_lane"
                                                                                       : "device_to_host_lane";
    const bool shared = (kind == "prefetch" && model.resource_lanes.shared_storage_read)
                        || (kind == "write_host_to_storage" && model.resource_lanes.shared_storage_write);
    return shared ? local : scope + "/" + local;
}

} // namespace markov::trace_graph::modules::hicache
