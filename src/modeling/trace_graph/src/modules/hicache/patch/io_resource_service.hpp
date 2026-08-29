/**
 * @file
 * @brief Small interpolation helpers for the compact I/O cost model.
 */
#pragma once

#include "io_resource_demand.hpp"

#include <string>

namespace markov::trace_graph::modules::hicache::patch::io_resource_model_detail {

struct NewWriteServiceParameters {
    double setup_us_per_operation = 0.0;
    double bandwidth_bytes_per_sec = 0.0;
};

[[nodiscard]] double interpolated_page_bandwidth(const std::vector<frontend::HiCacheIoPageBandwidthPoint> & points, double page_bytes);
[[nodiscard]] NewWriteServiceParameters interpolated_new_write(const frontend::HiCacheIoServiceModelConfig & model, double page_bytes);
[[nodiscard]] double interpolated_existing_key_bandwidth(const frontend::HiCacheIoServiceModelConfig & model, double page_bytes, double operation_pages);
[[nodiscard]] std::string resource_lane(const model::HiCacheEffectDecision & decision, const frontend::HiCacheIoCostConfig & model);

} // namespace markov::trace_graph::modules::hicache::patch::io_resource_model_detail
