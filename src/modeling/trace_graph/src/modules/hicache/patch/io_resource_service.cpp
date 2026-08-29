/**
 * @file
 * @brief Log-coordinate interpolation for physical I/O calibration curves.
 */
#include "io_resource_service.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <ranges>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::patch::io_resource_model_detail {

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

} // namespace

double interpolated_page_bandwidth(const std::vector<frontend::HiCacheIoPageBandwidthPoint> & points, double page_bytes) {
    std::vector<std::pair<double, double>> curve;
    curve.reserve(points.size());
    for (const auto & point : points) curve.emplace_back(static_cast<double>(point.page_bytes), point.bandwidth_bytes_per_sec);
    return log_curve(curve, page_bytes);
}

NewWriteServiceParameters interpolated_new_write(const frontend::HiCacheIoServiceModelConfig & model, double page_bytes) {
    std::vector<std::pair<double, double>> setup;
    std::vector<std::pair<double, double>> bandwidth;
    setup.reserve(model.new_operation_points.size());
    bandwidth.reserve(model.new_operation_points.size());
    for (const auto & point : model.new_operation_points) {
        setup.emplace_back(static_cast<double>(point.page_bytes), point.setup_us_per_operation);
        bandwidth.emplace_back(static_cast<double>(point.page_bytes), point.bandwidth_bytes_per_sec);
    }
    return {
        .setup_us_per_operation = linear_value_on_log_coordinate(setup, page_bytes),
        .bandwidth_bytes_per_sec = log_curve(bandwidth, page_bytes),
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

std::string resource_lane(const model::HiCacheEffectDecision & decision, const frontend::HiCacheIoCostConfig & model) {
    using model::HiCacheTransferDirection;
    const auto local = decision.direction == HiCacheTransferDirection::StorageToHost   ? "host_storage_read_lane"
                       : decision.direction == HiCacheTransferDirection::HostToStorage ? "host_storage_write_lane"
                       : decision.direction == HiCacheTransferDirection::HostToDevice  ? "host_to_device_lane"
                                                                                       : "device_to_host_lane";
    const bool shared = (decision.direction == HiCacheTransferDirection::StorageToHost && model.resource_lanes.shared_storage_read)
                        || (decision.direction == HiCacheTransferDirection::HostToStorage && model.resource_lanes.shared_storage_write);
    return shared ? local : decision.cache_scope + "/" + local;
}

} // namespace markov::trace_graph::modules::hicache::patch::io_resource_model_detail
