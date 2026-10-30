#include "markov/adapters/costs.hpp"
#include "markov/trace_graph/source/chrome_reader.hpp"

#include <cmath>
#include <fstream>
#include <limits>
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

void fields(const Json & object, const std::set<std::string> & required, const std::set<std::string> & optional = {}) {
    if (!object.is_object()) throw std::runtime_error("Cost input must be a JSON object");
    for (const auto & [key, value] : object.items()) {
        if (!required.contains(key) && !optional.contains(key)) throw std::runtime_error("Unknown cost input field: " + key);
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

cost::Affine affine(const Json & raw) {
    const auto intercept = raw.at("intercept_ns").get<double>();
    if (!std::isfinite(intercept)) throw std::runtime_error("Cost intercept must be finite");
    return { intercept, features(raw.at("coefficients_ns")) };
}

cost::Formula formula(const Json & raw) {
    const auto form = raw.at("form").get<std::string>();
    if (form == "affine") {
        fields(raw, { "form", "intercept_ns", "coefficients_ns" });
        return affine(raw);
    }
    if (form != "coefficient_curve") throw std::runtime_error("Unsupported cost response form: " + form);
    fields(raw, { "form", "feature", "points" });
    cost::CoefficientCurve result{ raw.at("feature").get<std::string>(), {} };
    const auto & points = raw.at("points");
    if (!points.is_array() || points.empty()) throw std::runtime_error("Cost coefficient curve requires a measured anchor");
    for (const auto & point : points) {
        fields(point, { "coordinate", "intercept_ns", "coefficients_ns" });
        const auto coordinate = point.at("coordinate").get<double>();
        if (!std::isfinite(coordinate) || (!result.points.empty() && coordinate <= result.points.back().coordinate))
            throw std::runtime_error("Cost curve coordinates must be finite and strictly increasing");
        result.points.push_back({ coordinate, affine(point) });
    }
    return result;
}

cost::Model model(const Json & raw) {
    fields(raw,
           { "operation",
             "implementation",
             "timing_scope",
             "response",
             "ranges",
             "measured_points",
             "extrapolation_features",
             "allow_new_combinations",
             "sources" },
           { "limitations" });
    cost::Model result{
        { raw.at("operation").get<std::string>(), raw.at("implementation").get<std::string>(), raw.at("timing_scope").get<std::string>() },
        formula(raw.at("response")),
        {},
        {},
        raw.at("extrapolation_features").get<std::set<std::string>>(),
        raw.at("allow_new_combinations").get<bool>(),
        raw.at("sources").get<std::vector<std::string>>()
    };
    if (result.sources.empty()) throw std::runtime_error("Cost model needs measurement sources");
    result.limitations = raw.value("limitations", std::vector<std::string>{});

    for (const auto & [name, values] : raw.at("ranges").get<std::map<std::string, std::vector<double>>>()) {
        if (values.size() != 2 || !std::isfinite(values[0]) || !std::isfinite(values[1]) || values[0] > values[1])
            throw std::runtime_error("Invalid cost domain range: " + name);
        result.ranges.emplace(name, cost::Range{ values[0], values[1] });
    }
    if (const auto * response = std::get_if<cost::Affine>(&result.response)) {
        for (const auto & [name, coefficient] : response->coefficients_ns) result.ranges.at(name);
    }
    else {
        const auto & curve = std::get<cost::CoefficientCurve>(result.response);
        const auto domain = result.ranges.at(curve.feature);
        if (domain.lower < curve.points.front().coordinate || domain.upper > curve.points.back().coordinate
            || result.extrapolation_features.contains(curve.feature))
            throw std::runtime_error("Cost curve domain must remain within its anchors: " + curve.feature);
        for (const auto & point : curve.points) {
            for (const auto & [name, coefficient] : point.response.coefficients_ns) result.ranges.at(name);
        }
    }
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

class CpuCostReader final : public source::RecordConsumer {
public:
    explicit CpuCostReader(const source::Observations & observations) {
        for (source::Index id = 0; id < observations.resources.size(); ++id) {
            const auto & resource = observations.resources[id];
            if (resource.kind == source::ResourceKind::CpuThread) resources_.emplace(resource.identity, id);
        }
    }

    void record(std::size_t, const Json & row) override {
        fields(row, { "resource", "begin_ns", "end_ns", "cost_ns" });
        const auto identity = row.at("resource").get<std::string>();
        const auto resource = resources_.find(identity);
        if (resource == resources_.end()) throw std::runtime_error("CPU correction resource is absent from source: " + identity);
        for (const auto * name : { "begin_ns", "end_ns", "cost_ns" }) {
            const auto & value = row.at(name);
            if (!value.is_number_integer() || value < 0 || value > std::numeric_limits<source::Time>::max())
                throw std::runtime_error("CPU correction requires nonnegative integer nanoseconds: " + std::string(name));
        }
        intervals[resource->second].push_back({ row.at("begin_ns"), row.at("end_ns"), row.at("cost_ns") });
    }

    std::map<source::Index, std::vector<source::CpuCostSpan>> intervals;

private:
    std::map<std::string, source::Index> resources_;
};

} // namespace

SourceCpuCosts read_source_cpu_costs(const std::filesystem::path & path, const source::ProfileManifest & manifest, const source::Observations & observations) {
    CpuCostReader reader(observations);
    auto parsed = source::read_json_records(path, "intervals", reader);
    const auto & raw = parsed.metadata;
    fields(raw, { "source_manifest", "basis", "sources", "intervals" });
    auto captured = std::filesystem::path(raw.at("source_manifest").get<std::string>());
    if (captured.is_relative()) captured = path.parent_path() / captured;
    if (std::filesystem::absolute(captured).lexically_normal() != manifest.path)
        throw std::runtime_error("CPU corrections belong to a different source manifest");
    const auto basis = raw.at("basis").get<std::string>();
    const auto sources = raw.at("sources").get<std::vector<std::string>>();
    if (basis.empty() || sources.empty() || parsed.count == 0 || !raw.at("intervals").is_array() || !raw.at("intervals").empty())
        throw std::runtime_error("CPU corrections require provenance and interval records");
    return { path, basis, sources, source::CpuCosts(std::move(reader.intervals)) };
}

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
