#include "frontend/cost_package.hpp"

#include "markov/trace_graph/frontend/model_config_parse_detail.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace markov::trace_graph::frontend {
namespace {
using Json = nlohmann::json;
using model_config_detail::number_value;
using model_config_detail::read_json_file;
using model_config_detail::u64_value;

std::shared_ptr<const cost::OperatorModels> parse_operator_models(const Json & items) {
    if (!items.is_array()) throw std::runtime_error("operator_models must be an array");
    std::vector<cost::OperatorModel> models;
    models.reserve(items.size());
    std::set<std::tuple<std::string, std::string, std::string>> identities;

    for (const auto & item : items) {
        cost::OperatorModel model;
        model.operation = item.at("operation").get<std::string>();
        model.implementation = item.at("implementation").get<std::string>();
        model.timing_scope = item.at("timing_scope").get<std::string>();
        model.form = item.at("form").get<std::string>();
        model.allow_extrapolation = item.at("allow_extrapolation").get<bool>();
        model.extrapolation_features = item.value("extrapolation_features", std::vector<std::string>{});
        model.feature_ranges = item.at("feature_ranges").get<decltype(model.feature_ranges)>();
        model.measured_feature_points = item.at("measured_feature_points").get<decltype(model.measured_feature_points)>();
        if (model.form == "piecewise_linear") {
            model.coordinate = item.at("coordinate").get<std::string>();
            model.points = item.at("points").get<decltype(model.points)>();
            if (model.allow_extrapolation || !model.extrapolation_features.empty() || model.points.size() < 2 || !std::ranges::is_sorted(model.points))
                throw std::runtime_error("Operator curve needs ordered anchors and cannot extrapolate");
            for (const auto & [coordinate, duration_us] : model.points)
                if (!std::isfinite(coordinate) || !std::isfinite(duration_us) || duration_us < 0.0)
                    throw std::runtime_error("Operator curve needs finite coordinates and non-negative durations");
            if (!model.feature_ranges.contains(model.coordinate)) throw std::runtime_error("Operator curve needs coverage for its coordinate");
            const auto range = model.feature_ranges.at(model.coordinate);
            if (range.first != model.points.front().first || range.second != model.points.back().first)
                throw std::runtime_error("Operator curve coverage must match its measured anchors");
            for (size_t i = 1; i < model.points.size(); ++i)
                if (model.points[i - 1].first >= model.points[i].first) throw std::runtime_error("Operator curve coordinates must be distinct");
        }
        else if (model.form == "linear" || model.form == "constant") {
            model.intercept_us = item.at("intercept_us").get<double>();
            model.coefficients = item.at("coefficients").get<decltype(model.coefficients)>();
            if (model.form == "constant" && !model.coefficients.empty())
                throw std::runtime_error("Constant operator models cannot contain variable coefficients");
        }
        else throw std::runtime_error("Unsupported operator model form: " + model.form);

        for (const auto & [feature, range] : model.feature_ranges)
            if (!std::isfinite(range.first) || !std::isfinite(range.second) || range.first > range.second)
                throw std::runtime_error("Invalid operator coverage for " + feature);
        for (const auto & feature : model.extrapolation_features)
            if (!model.feature_ranges.contains(feature)) throw std::runtime_error("Operator extrapolation needs a declared coordinate: " + feature);
        if (!std::isfinite(model.intercept_us)) throw std::runtime_error("Operator intercept must be finite");
        for (const auto & [feature, value] : model.coefficients)
            if (!std::isfinite(value) || !model.feature_ranges.contains(feature))
                throw std::runtime_error("Operator coefficient needs a finite value and measured coverage: " + feature);
        if (model.measured_feature_points.empty()) throw std::runtime_error("Operator model needs at least one measured work point");
        for (const auto & point : model.measured_feature_points) {
            if (point.size() != model.feature_ranges.size()) throw std::runtime_error("Operator work point must cover its declared coordinates");
            for (const auto & [feature, range] : model.feature_ranges) {
                const auto coordinate = point.at(feature);
                if (!std::isfinite(coordinate) || coordinate < range.first || coordinate > range.second)
                    throw std::runtime_error("Operator work point lies outside its declared coverage");
            }
        }
        if (!identities.emplace(model.operation, model.implementation, model.timing_scope).second)
            throw std::runtime_error("Operator cost package contains overlapping model identities");

        models.push_back(std::move(model));
    }
    return std::make_shared<const cost::OperatorModels>(std::move(models));
}

void validate_scale_points(const Json & points) {
    if (!points.is_array() || points.empty()) throw std::runtime_error("Runtime scale requires measured anchors");
    uint64_t previous = 0;
    for (const auto & point : points) {
        model_config_detail::require_exact_fields(point, "runtime_scale_points", { "page_bytes", "runtime_scale" });
        const auto page_bytes = u64_value(point, "page_bytes", 0);
        if (page_bytes <= previous || number_value(point, "runtime_scale", 0) <= 0)
            throw std::runtime_error("Runtime scales must be positive with increasing page anchors");
        previous = page_bytes;
    }
}

double scale_for_page(const Json & points, uint64_t page_bytes) {
    // Keep the package projection's ratio arithmetic so integer rounding stays unchanged.
    const auto coordinate = static_cast<double>(page_bytes);
    if (coordinate <= points.front().at("page_bytes").get<double>()) return points.front().at("runtime_scale").get<double>();
    if (coordinate >= points.back().at("page_bytes").get<double>()) return points.back().at("runtime_scale").get<double>();

    const auto right = std::ranges::lower_bound(points, coordinate, {}, [](const auto & point) { return point.at("page_bytes").template get<double>(); });
    const auto left = std::prev(right);
    const auto position =
        std::log(coordinate / left->at("page_bytes").get<double>()) / std::log(right->at("page_bytes").get<double>() / left->at("page_bytes").get<double>());
    const auto left_value = left->at("runtime_scale").get<double>();
    return std::exp(std::log(left_value) + position * std::log(right->at("runtime_scale").get<double>() / left_value));
}

} // namespace

HiCacheIoCostConfig CostPackage::io_for_page(uint64_t page_bytes) const {
    auto projected = io;
    for (auto & [kind, model] : projected.at("service_models").items()) {
        const bool existing_write = kind == "write_host_to_storage";
        const auto field = existing_write ? "existing_runtime_scale_points" : "runtime_scale_points";
        if (!model.contains(field)) continue;

        model[existing_write ? "existing_runtime_scale" : "runtime_scale"] = scale_for_page(model.at(field), page_bytes);
        model.erase(field);
    }
    return model_config_detail::parse_hicache_io_cost(Json{
        { "io_cost", std::move(projected) }
    });
}

const CostPackage & CostPackages::get(const std::string & filename) {
    const auto path = std::filesystem::canonical(filename).string();
    if (const auto found = packages_.find(path); found != packages_.end()) return found->second;

    const auto raw = read_json_file(path);
    const auto & io = raw.at("hicache");
    model_config_detail::require_exact_fields(io,
                                              "cost package hicache",
                                              { "storage_batch_pages", "kv_bytes_per_token_per_rank", "resource_lanes", "service_models" });
    const auto kv_bytes = u64_value(io, "kv_bytes_per_token_per_rank", 0);
    if (kv_bytes == 0) throw std::runtime_error("Cost package kv_bytes_per_token_per_rank must be positive");
    CostPackage package{
        .kv_bytes_per_token = kv_bytes,
        .io = { { "storage_batch_pages", io.at("storage_batch_pages") },
               { "resource_lanes", io.at("resource_lanes") },
               { "service_models", io.at("service_models") } },
        .operators = parse_operator_models(raw.at("operator_models")),
    };
    for (const auto & [kind, model] : package.io.at("service_models").items()) {
        const bool existing_write = kind == "write_host_to_storage";
        const auto field = existing_write ? "existing_runtime_scale_points" : "runtime_scale_points";
        if (!existing_write || model.contains("existing_key_bandwidth_points")) validate_scale_points(model.at(field));
    }
    // Validate the remaining service and resource fields at admission, before trace loading.
    package.io_for_page(kv_bytes);
    return packages_.emplace(path, std::move(package)).first->second;
}

} // namespace markov::trace_graph::frontend
