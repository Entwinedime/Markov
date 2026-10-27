#include "markov/adapters/costs.hpp"

#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace markov::adapters {
namespace {

using Json = nlohmann::json;
using namespace trace_graph;

Json read_json(const std::filesystem::path & path) {
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("Cannot read cost input: " + path.string());
    return Json::parse(stream);
}

void fields(const Json & object, const std::set<std::string> & required) {
    if (!object.is_object()) throw std::runtime_error("Cost input must be a JSON object");
    for (const auto & [key, value] : object.items()) {
        if (!required.contains(key)) throw std::runtime_error("Unknown cost input field: " + key);
    }
    for (const auto & key : required) object.at(key);
}

Json deployment(const Json & raw) {
    fields(raw, { "platform", "framework", "runtime", "model", "dtype", "tensor_parallel" });
    for (const auto * key : { "platform", "framework", "runtime", "model", "dtype" }) {
        if (raw.at(key).get<std::string>().empty()) throw std::runtime_error("Empty cost deployment condition: " + std::string(key));
    }
    if (!raw.at("tensor_parallel").is_number_integer() || raw.at("tensor_parallel").get<std::int64_t>() < 1)
        throw std::runtime_error("Cost deployment tensor_parallel must be a positive integer");
    return raw;
}

planning::Features features(const Json & raw) {
    const auto result = raw.get<planning::Features>();
    for (const auto & [name, value] : result) {
        if (!std::isfinite(value)) throw std::runtime_error("Non-finite cost feature: " + name);
    }
    return result;
}

cost::Model model(const Json & raw) {
    fields(raw,
           { "operation",
             "implementation",
             "timing_scope",
             "intercept_ns",
             "coefficients_ns",
             "ranges",
             "measured_points",
             "extrapolation_features",
             "allow_new_combinations",
             "sources" });
    cost::Model result{
        { raw.at("operation").get<std::string>(), raw.at("implementation").get<std::string>(), raw.at("timing_scope").get<std::string>() },
        raw.at("intercept_ns").get<double>(),
        features(raw.at("coefficients_ns")),
        {},
        {},
        raw.at("extrapolation_features").get<std::set<std::string>>(),
        raw.at("allow_new_combinations").get<bool>(),
        raw.at("sources").get<std::vector<std::string>>()
    };
    if (!std::isfinite(result.intercept_ns) || result.sources.empty()) throw std::runtime_error("Cost model needs finite coefficients and measurement sources");

    for (const auto & [name, values] : raw.at("ranges").get<std::map<std::string, std::vector<double>>>()) {
        if (values.size() != 2 || !std::isfinite(values[0]) || !std::isfinite(values[1]) || values[0] > values[1])
            throw std::runtime_error("Invalid cost domain range: " + name);
        result.ranges.emplace(name, cost::Range{ values[0], values[1] });
    }
    for (const auto & [name, coefficient] : result.coefficients_ns) result.ranges.at(name);
    for (const auto & name : result.extrapolation_features) result.ranges.at(name);

    if (!raw.at("measured_points").is_array()) throw std::runtime_error("Cost measured_points must be an array");
    for (const auto & point : raw.at("measured_points")) {
        const auto values = features(point);
        if (values.size() != result.ranges.size()) throw std::runtime_error("Measured work point does not match its declared feature domain");

        std::vector<double> coordinates;
        for (const auto & [name, range] : result.ranges) {
            const auto value = values.at(name);
            if (value < range.lower || value > range.upper) throw std::runtime_error("Measured work point is outside its declared range: " + name);
            coordinates.push_back(value);
        }
        result.measured_points.insert(std::move(coordinates));
    }
    return result;
}

} // namespace

SelectedCosts read_cost_models(const std::filesystem::path & path) {
    const auto raw = read_json(path);
    fields(raw, { "deployment", "models" });
    if (!raw.at("models").is_array()) throw std::runtime_error("Cost models must be an array");
    std::vector<cost::Model> models;
    for (const auto & entry : raw.at("models")) models.push_back(model(entry));
    return { deployment(raw.at("deployment")), cost::Models(std::move(models)) };
}

CostQueries read_cost_queries(const std::filesystem::path & path) {
    const auto raw = read_json(path);
    fields(raw, { "deployment", "models", "work" });
    if (!raw.at("work").is_array()) throw std::runtime_error("Cost work queries must be an array");
    auto models_path = std::filesystem::path(raw.at("models").get<std::string>());
    if (models_path.is_relative()) models_path = path.parent_path() / models_path;

    CostQueries result{ std::filesystem::absolute(models_path).lexically_normal(), deployment(raw.at("deployment")), {} };
    for (const auto & item : raw.at("work")) {
        fields(item, { "operation", "implementation", "timing_scope", "features" });
        result.work.push_back({
            { item.at("operation").get<std::string>(), item.at("implementation").get<std::string>(), item.at("timing_scope").get<std::string>() },
            features(item.at("features"))
        });
    }
    return result;
}

} // namespace markov::adapters
