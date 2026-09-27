/**
 * @file
 * @brief One service formula shared by asynchronous state and DAG assembly.
 */
#include "markov/trace_graph/modules/hicache/service_model.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/missing_cost.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <ranges>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache {

namespace {

double interpolate_curve(const auto & points, double coordinate, auto axis, auto value, bool logarithmic_value = true) {
    if (points.empty() || coordinate <= 0.0) return 0.0;
    if (coordinate <= axis(points.front())) return value(points.front());
    if (coordinate >= axis(points.back())) return value(points.back());
    for (size_t index = 1; index < points.size(); ++index) {
        const auto & left = points[index - 1];
        const auto & right = points[index];
        if (coordinate > axis(right)) continue;
        const auto position = (std::log(coordinate) - std::log(axis(left))) / (std::log(axis(right)) - std::log(axis(left)));
        if (logarithmic_value) return std::exp(std::log(value(left)) + position * (std::log(value(right)) - std::log(value(left))));
        return value(left) + position * (value(right) - value(left));
    }
    return 0.0;
}

std::pair<double, double> interpolated_transfer(const std::vector<frontend::HiCacheIoTransferPoint> & points, double page_bytes) {
    const auto axis = [](const auto & point) { return static_cast<double>(point.page_bytes); };
    return {
        interpolate_curve(points, page_bytes, axis, [](const auto & point) { return point.setup_us_per_operation; }, false),
        interpolate_curve(points, page_bytes, axis, [](const auto & point) { return point.bandwidth_bytes_per_sec; }),
    };
}

double interpolated_existing_key_bandwidth(const frontend::HiCacheIoServiceModelConfig & model, double page_bytes, double operation_pages) {
    std::map<uint64_t, std::vector<std::pair<double, double>>> by_page;
    for (const auto & point : model.existing_key_bandwidth_points)
        by_page[point.page_bytes].emplace_back(static_cast<double>(point.operation_pages), point.bandwidth_bytes_per_sec);
    std::vector<std::pair<double, double>> page_curve;
    page_curve.reserve(by_page.size());
    const auto axis = [](const auto & point) { return point.first; };
    const auto value = [](const auto & point) { return point.second; };
    for (auto & [calibrated_page, operation_curve] : by_page) {
        std::ranges::sort(operation_curve, {}, &std::pair<double, double>::first);
        page_curve.emplace_back(static_cast<double>(calibrated_page), interpolate_curve(operation_curve, operation_pages, axis, value));
    }
    return interpolate_curve(page_curve, page_bytes, axis, value);
}

} // namespace

const frontend::HiCacheIoServiceModelConfig & require_hicache_service(const frontend::HiCacheIoCostConfig & config, const std::string & kind,
                                                                      uint64_t page_bytes, uint64_t pages, uint64_t existing_pages) {
    const auto found = config.service_models.find(kind);
    if (found == config.service_models.end()) {
        std::string component = "physical/" + kind;
        if (kind == "prefetch") component = "physical/prefetch_stages";
        if (kind == "write_host_to_storage") component = existing_pages ? "physical/write_host_to_storage_existing" : "service/write_host_to_storage_new";
        throw MissingCostEvidence(component,
                                  {
                                      { "page_bytes", page_bytes },
                                      { "page_count",      pages }
        },
                                  "Target transfer requires a measured or base-derived " + kind + " service model");
    }
    return found->second;
}

std::optional<HiCacheServiceCost> hicache_service_cost(const frontend::HiCacheIoServiceModelConfig & model, uint64_t page_bytes, uint64_t pages, uint64_t calls,
                                                       uint64_t existing_pages, std::optional<uint64_t> copied_pages) {
    HiCacheServiceCost cost;
    if (pages == 0) return cost;
    if (page_bytes == 0 || calls == 0 || existing_pages > pages) return std::nullopt;
    if (copied_pages && (model.direction != "storage_to_host" || !model.stages || *copied_pages == 0 || *copied_pages > pages)) return std::nullopt;
    double duration = 0.0;
    if (model.direction == "host_to_storage") {
        if (existing_pages > 0) {
            if (model.existing_key_bandwidth_points.empty())
                throw MissingCostEvidence("physical/write_host_to_storage_existing",
                                          {
                                              {     "page_bytes",     page_bytes },
                                              {     "page_count",          pages },
                                              { "existing_pages", existing_pages }
                },
                                          "Storage batch requires existing-key materialization costs");
            const auto bandwidth = interpolated_existing_key_bandwidth(model, page_bytes, pages);
            if (bandwidth <= 0.0) return std::nullopt;
            cost.transfer_us = static_cast<double>(existing_pages) * page_bytes * 1'000'000.0 / bandwidth;
            cost.existing_us = cost.transfer_us * model.existing_runtime_scale;
        }
        if (pages > existing_pages) {
            if (model.new_operation_points.empty())
                throw MissingCostEvidence("service/write_host_to_storage_new",
                                          {
                                              { "page_bytes",             page_bytes },
                                              { "page_count",                  pages },
                                              {  "new_pages", pages - existing_pages }
                },
                                          "Storage batch requires new-key setup and transfer costs");
            const auto [setup, bandwidth] = interpolated_transfer(model.new_operation_points, page_bytes);
            if (bandwidth <= 0.0) return std::nullopt;
            cost.setup_us = setup * static_cast<double>(calls);
            const auto transfer = static_cast<double>(pages - existing_pages) * page_bytes * 1'000'000.0 / bandwidth;
            cost.transfer_us += transfer;
            cost.new_us = cost.setup_us + transfer;
        }
        duration = cost.existing_us + cost.new_us;
    }
    else if (model.direction == "storage_to_host") {
        if (!model.stages) return std::nullopt;
        const auto & stages = *model.stages;
        const auto copied = copied_pages.value_or(pages);
        cost.setup_us = stages.return_us_per_operation * calls + (stages.before_copy_us_per_page + stages.return_us_per_page) * pages
                        + stages.copy_publish_us_per_page * copied;
        cost.transfer_us = (stages.before_copy_us_per_byte * pages + stages.copy_publish_us_per_byte * copied) * page_bytes;
        const auto per_byte = stages.before_copy_us_per_byte + stages.copy_publish_us_per_byte;
        if (per_byte > 0) cost.bandwidth_bytes_per_sec = 1'000'000.0 / (per_byte * model.runtime_scale);
        duration = ((stages.before_copy_us_per_page + stages.before_copy_us_per_byte * page_bytes) * pages
                    + (stages.copy_publish_us_per_page + stages.copy_publish_us_per_byte * page_bytes) * copied + stages.return_us_per_operation * calls
                    + stages.return_us_per_page * pages)
                   * model.runtime_scale;
    }
    else {
        const auto [setup, bandwidth] = interpolated_transfer(model.page_bandwidth_points, page_bytes);
        if (bandwidth <= 0.0) return std::nullopt;
        cost.bandwidth_bytes_per_sec = bandwidth / model.runtime_scale;
        cost.setup_us = setup * static_cast<double>(calls);
        cost.transfer_us = static_cast<double>(pages) * page_bytes * 1'000'000.0 / bandwidth;
        duration = (cost.setup_us + cost.transfer_us) * model.runtime_scale;
    }
    if (!std::isfinite(duration) || duration < 0.0 || duration >= static_cast<double>(std::numeric_limits<uint64_t>::max())) return std::nullopt;
    cost.duration_us = static_cast<uint64_t>(std::ceil(duration));
    return cost;
}

std::optional<PrefetchControlBatch> hicache_prefetch_batch(const frontend::HiCacheIoServiceModelConfig & model, uint64_t page_bytes, uint64_t pages,
                                                           uint64_t start_us) {
    if (model.direction != "storage_to_host" || !model.stages || pages == 0) return std::nullopt;
    const auto cost = hicache_service_cost(model, page_bytes, pages);
    if (!cost) return std::nullopt;
    const auto & stages = *model.stages;
    const auto before = (stages.before_copy_us_per_page + stages.before_copy_us_per_byte * page_bytes) * pages;
    const auto step = stages.copy_publish_us_per_page + stages.copy_publish_us_per_byte * page_bytes;
    const auto at = [&](double duration) {
        return core::checked_add_u64(start_us, static_cast<uint64_t>(std::ceil(duration * model.runtime_scale)), "prefetch service time overflow");
    };
    PrefetchControlBatch batch{ .page_count = pages,
                                .start_ts = start_us,
                                .ready_ts = core::checked_add_u64(start_us, cost->duration_us, "prefetch service time overflow") };
    for (uint64_t index = 1; index <= pages; ++index) {
        const auto publication = before + step * index;
        const auto cancelled_cost = hicache_service_cost(model, page_bytes, pages, 1, 0, index);
        batch.publications.push_back({ 1, at(publication), core::checked_add_u64(start_us, cancelled_cost->duration_us, "prefetch service time overflow") });
    }
    return batch;
}

PrefetchBatchExecution execute_prefetch_batch(const PrefetchControlBatch & batch, std::optional<uint64_t> stop_us) {
    PrefetchBatchExecution result{ .release_us = batch.ready_ts };
    for (const auto & publication : batch.publications) {
        result.copied_pages = core::checked_add_u64(result.copied_pages, publication.page_count, "prefetch copied pages overflow");
        if (stop_us && publication.at_us >= *stop_us) {
            if (!publication.cancelled_return_us) throw std::invalid_argument("cancelled prefetch requires a measured return-path timing");
            result.release_us = *publication.cancelled_return_us;
            result.cancelled = true;
            break;
        }
        result.published_pages = result.copied_pages;
    }
    return result;
}

std::string hicache_resource_lane(const frontend::HiCacheIoCostConfig & model, const std::string & kind, const std::string & scope) {
    const auto local = kind == "prefetch"                ? "host_storage_read_lane"
                       : kind == "write_host_to_storage" ? "host_storage_write_lane"
                       : kind == "load"                  ? "host_to_device_lane"
                                                         : "device_to_host_lane";
    const bool shared =
        (kind == "prefetch" && model.resource_lanes.shared_storage_read) || (kind == "write_host_to_storage" && model.resource_lanes.shared_storage_write);
    return shared ? local : scope + "/" + local;
}

} // namespace markov::trace_graph::modules::hicache
