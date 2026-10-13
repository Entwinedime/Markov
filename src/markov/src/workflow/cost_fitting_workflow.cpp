#include "markov/workflow/cost_fitting_workflow.hpp"

#include "markov/contracts/json_input.hpp"
#include "markov/performance/admission_cost_fitter.hpp"
#include "markov/performance/communication_cost_fitter.hpp"
#include "markov/performance/kernel_cost_fitter.hpp"
#include "markov/performance/worker_cost_fitter.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace markov {

struct CostFittingWorkflow::Measurements {
    std::vector<AdmissionCostSample> samples;
    std::map<std::string, std::size_t> excluded;
    nlohmann::json manifest;
    nlohmann::json context;
    nlohmann::json sources;
    std::vector<WorkerCostSample> worker_samples;
    std::vector<KernelCostSample> kernel_samples;
    std::vector<CommunicationCostSample> communication_samples;
    nlohmann::json readiness;
    nlohmann::json gaps;
};

CostFittingWorkflow::Measurements CostFittingWorkflow::read(const std::filesystem::path & manifest_path, std::string_view unit) const {
    if (unit != "admission" && unit != "worker" && unit != "kernel" && unit != "communication")
        throw std::invalid_argument("unknown cost unit: " + std::string(unit));
    const auto manifest = input::read_json(manifest_path);
    const bool device = unit == "kernel" || unit == "communication";
    const auto & kind = manifest.at("measurement_kind");
    const bool matching =
        device ? kind == "native_device_work" || (unit == "communication" && kind == "native_collective_work") : kind == "native_serving_timing";
    if (manifest.at("role") != "performance" || !matching) throw std::invalid_argument("cost fitting requires independent measurements for the selected unit");

    const auto prepared = input::read_json(input::resolve_path(manifest_path, manifest.at("input")));
    const auto environment = input::read_json(input::resolve_path(manifest_path, manifest.at("environment")));
    Measurements data;
    data.manifest = manifest;
    data.sources = nlohmann::json::array();
    data.context = {
        {         "framework",         environment.at("framework") },
        { "framework_version", environment.at("framework_version") },
        {        "model_path",           prepared.at("model_path") },
        {     "configuration",        prepared.at("configuration") },
        {        "deployment",          prepared.at("environment") },
        {           "runtime",           environment.at("runtime") }
    };
    if (device) {
        data.readiness = manifest.at("readiness");
        data.gaps = manifest.at("gaps");
        read_device(data, manifest_path, unit);
        return data;
    }

    auto workers = nlohmann::json::array();
    for (const auto & reference : manifest.at("worker_environments")) {
        const auto worker = input::read_json(input::resolve_path(manifest_path, reference));
        workers.push_back({
            {         "rank",         worker.at("rank") },
            { "cpu_affinity", worker.at("cpu_affinity") },
            {      "runtime",      worker.at("runtime") }
        });
    }

    data.context["cpu_count"] = environment.at("cpu_count");
    data.context["workers"] = std::move(workers);
    data.readiness = manifest.at(unit == "worker" ? "cpu_worker_readiness" : "cpu_unit_readiness");
    data.gaps = manifest.at(unit == "worker" ? "cpu_worker_gaps" : "cpu_unit_gaps");
    if (unit == "worker") read_workers(data, manifest_path);
    else read_admission(data, manifest_path);
    return data;
}

void CostFittingWorkflow::read_admission(Measurements & data, const std::filesystem::path & manifest_path) const {
    for (const auto & reference : data.manifest.at("cpu_units")) {
        const auto source = std::filesystem::weakly_canonical(input::resolve_path(manifest_path, reference));
        data.sources.push_back(source.string());
        std::ifstream stream(source);
        stream.exceptions(std::ios::badbit);
        if (!stream.is_open()) throw std::runtime_error("cannot open CPU unit measurements: " + reference.get<std::string>());

        std::string line;
        while (std::getline(stream, line)) {
            const auto record = nlohmann::json::parse(line);
            if (record.at("stage") == "warmup") {
                ++data.excluded["warmup"];
                continue;
            }
            if (record.at("stage") != "measurement") throw std::invalid_argument("unknown CPU measurement stage");
            if (record.at("unit") != "admission_budget") {
                ++data.excluded["unit_not_implemented"];
                continue;
            }
            const auto & parameters = record.at("parameters");
            if (!record.at("candidate_accepted").get<bool>() || parameters.at("horizon_checks").is_null()) {
                ++data.excluded["accepted_full_horizon_not_observed"];
                continue;
            }

            constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
            const AdmissionServiceWork work{ static_cast<std::size_t>(input::integer(parameters.at("accepted_requests"), 0, maximum)),
                                             static_cast<std::size_t>(input::integer(parameters.at("running_requests"), 0, maximum)),
                                             input::positive(parameters.at("input_tokens")),
                                             static_cast<std::size_t>(input::integer(parameters.at("horizon_checks"), 1, maximum)) };
            if (record.at("request_states_initialized").get<bool>() != work.request_states_initialized()) {
                ++data.excluded["initialization_path_differs"];
                continue;
            }
            if (input::integer(record.at("runtime_wait_cpu_ns"), 0, maximum) != 0 || input::integer(record.at("native_probe_cpu_ns"), 0, maximum) != 0) {
                ++data.excluded["nested_wait_or_probe_observed"];
                continue;
            }
            data.samples.push_back({ work, input::integer(record.at("native_call_cpu_ns"), 0, maximum) });
        }
    }
}

void CostFittingWorkflow::read_workers(Measurements & data, const std::filesystem::path & manifest_path) const {
    const auto path = input::resolve_path(manifest_path, data.manifest.at("cpu_worker_units"));
    const auto observations = input::read_json(path);
    if (observations.at("kind") != "cpu_worker_units" || observations.at("unit") != "ns")
        throw std::invalid_argument("worker fitting requires normalized CPU units in ns");

    // Include the raw files in overlap detection: publishing another normalized
    // file does not turn the same native observation into an independent sample.
    data.sources.push_back(std::filesystem::weakly_canonical(path).string());
    for (const auto & source : data.manifest.at("cpu_workers"))
        data.sources.push_back(std::filesystem::weakly_canonical(input::resolve_path(manifest_path, source)).string());

    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr CpuSubmissionPart parts[]{ CpuSubmissionPart::Before, CpuSubmissionPart::Call, CpuSubmissionPart::After };
    for (const auto & unit : observations.at("units")) {
        const auto & scope = observations.at("scopes").at(unit.at("scope").get<std::size_t>());
        if (scope.at("stage") == "warmup") {
            ++data.excluded["warmup"];
            continue;
        }
        if (scope.at("stage") != "measurement") throw std::invalid_argument("unknown worker measurement stage");
        if (unit.at("operation").is_null()) {
            ++data.excluded["worker_operation_unresolved"];
            continue;
        }

        const auto & worker = unit.at("worker");
        const auto & boundaries = worker.at("boundaries");
        const auto & segments = worker.at("segments");
        if (boundaries.size() != 4 || segments.size() != 3 || boundaries.at(0).at("boundary") != "begin" || boundaries.at(1).at("boundary") != "enter"
            || boundaries.at(2).at("boundary") != "exit" || boundaries.at(3).at("boundary") != "end"
            || boundaries.at(1).at("api") != boundaries.at(2).at("api")) {
            ++data.excluded["complete_single_submission_not_observed"];
            continue;
        }

        bool has_wait = false;
        std::array<std::int64_t, 3> services;
        for (std::size_t index = 0; index < segments.size(); ++index) {
            const auto & segment = segments[index];
            if (input::integer(segment.at("wait_cpu_ns"), 0, maximum) != 0) has_wait = true;

            const auto observed = input::integer(segment.at("thread_cpu_ns"), 0, maximum);
            const auto probe = input::integer(segment.at("probe_cpu_ns"), 0, maximum);
            if (probe > observed) throw std::invalid_argument("measured worker probes exceed the thread CPU interval");
            services[index] = observed - probe;
        }
        if (has_wait) {
            ++data.excluded["nested_wait_observed"];
            continue;
        }

        const auto operation = input::string_value(unit.at("operation"));
        for (std::size_t index = 0; index < segments.size(); ++index) data.worker_samples.push_back({ operation, parts[index], services[index] });
    }
}

void CostFittingWorkflow::read_device(Measurements & data, const std::filesystem::path & manifest_path, std::string_view unit) const {
    const auto field = data.manifest.at("measurement_kind") == "native_collective_work" ? "collective_measurements" : "device_measurements";
    const auto path = input::resolve_path(manifest_path, data.manifest.at(field));
    const auto observations = input::read_json(path);
    std::set<std::filesystem::path> sources{ path };

    // Hardware source references are provenance. Compare only recorded device
    // properties in the context, without making a portability assertion.
    auto devices = observations.at("devices");
    for (auto & device : devices) {
        device.erase("pid");
        device.erase("source");
    }
    data.context["devices"] = std::move(devices);

    const bool communication = unit == "communication";
    for (const auto & sample : observations.at(communication ? "task_samples" : "samples")) {
        sources.insert(input::resolve_path(path, sample.at("source").at("trace")));
        if (sample.at("stage") == "warmup") {
            ++data.excluded["warmup"];
            continue;
        }
        if (sample.at("stage") != "measurement") throw std::invalid_argument("unknown device measurement stage");
        if (communication) {
            if (sample.at("kind") != "dma" && sample.at("kind") != "device_marker") {
                ++data.excluded["not_communication_service"];
                continue;
            }

            data.communication_samples.push_back(
                { CommunicationCostFeatures::read(sample), input::integer(sample.at("observed_wall_ns"), 0, std::numeric_limits<std::int64_t>::max()) });
            continue;
        }
        if (sample.at("kind") != "compute") {
            ++data.excluded["not_compute"];
            continue;
        }
        if (sample.at("kernel_work").is_null()) {
            ++data.excluded["kernel_work_not_observed"];
            continue;
        }

        data.kernel_samples.push_back(
            { KernelCostFeatures::read(sample.at("kernel_work")), input::integer(sample.at("duration_ns"), 0, std::numeric_limits<std::int64_t>::max()) });
    }
    for (const auto & source : sources) data.sources.push_back(std::filesystem::weakly_canonical(source).string());
}

nlohmann::json CostFittingWorkflow::run(const std::filesystem::path & manifest_path, std::string_view unit) const {
    const auto data = read(manifest_path, unit);
    const bool worker = unit == "worker";
    nlohmann::json entries;
    std::string_view kind;
    std::vector<std::string> assumptions;
    if (unit == "communication") {
        entries = CommunicationCostFitter{}.fit(data.communication_samples);
        kind = "empirical_communication_median";
        assumptions = { "physical data and command tasks only; waits and collective envelopes excluded",
                        "physical device span; profiler overhead and internal transport stalls not removed",
                        "median rounded down to integral ns; no outlier removal",
                        "exact task kind, operation, bytes, dtype, transport and rank endpoints; no interpolation or extrapolation",
                        "recorded deployment only; concurrent contention and runtime transfer not established" };
    }
    else if (unit == "kernel") {
        entries = KernelCostFitter{}.fit(data.kernel_samples);
        kind = "empirical_kernel_median";
        assumptions = { "physical device span; profiler overhead and hidden stalls not removed",
                        "median rounded down to integral ns; no outlier removal",
                        "exact kernel, tensor and launch descriptors; no interpolation or extrapolation",
                        "data-dependent work, rank, phase and cache state omitted; independent validation required",
                        "recorded deployment only; runtime equivalence and target launch mapping not established" };
    }
    else {
        entries = worker ? WorkerCostFitter{}.fit(data.worker_samples) : AdmissionCostFitter{}.fit(data.samples);
        kind = worker ? "empirical_worker_median" : "empirical_admission_median";
        assumptions = { worker ? "adjacent worker thread CPU interval minus recorded paired probes; unmeasured observation overhead remains"
                               : "complete native call thread CPU; no clock or call-boundary correction",
                        "median rounded down to integral ns; no outlier removal",
                        worker ? "fixed model/deployment; complete one-submission workers only"
                               : "same deployment and accepted full-horizon path; no extrapolation",
                        worker ? "operation and part only; shape, rank and stage omitted; independent validation required"
                               : "input length and rank omitted from fitting keys; independent validation required" };
    }

    // Fitting never upgrades measurement readiness or absorbs waits into cost.
    return {
        {                  "kind",                                                                 kind },
        {                  "unit",                                                                 "ns" },
        {                "status",                         entries.empty() ? "needs_data" : "candidate" },
        {       "source_manifest", std::filesystem::absolute(manifest_path).lexically_normal().string() },
        {        "sample_sources",                                                         data.sources },
        {      "observed_context",                                                         data.context },
        { "measurement_readiness",                                                       data.readiness },
        {      "measurement_gaps",                                                            data.gaps },
        {               "entries",                                                              entries },
        {              "excluded",                                                        data.excluded },
        {           "assumptions",                                                          assumptions }
    };
}

nlohmann::json CostFittingWorkflow::evaluate(const std::filesystem::path & candidate_path, const std::filesystem::path & manifest_path) const {
    const auto candidate = input::read_json(candidate_path);
    const bool worker = candidate.at("kind") == "empirical_worker_median";
    const bool kernel = candidate.at("kind") == "empirical_kernel_median";
    const bool communication = candidate.at("kind") == "empirical_communication_median";
    if ((!worker && !kernel && !communication && candidate.at("kind") != "empirical_admission_median") || candidate.at("unit") != "ns"
        || candidate.at("status") != "candidate")
        throw std::invalid_argument("cost evaluation requires a measured median candidate in ns");

    std::string_view unit = "admission";
    if (worker) unit = "worker";
    else if (kernel) unit = "kernel";
    else if (communication) unit = "communication";
    const auto data = read(manifest_path, unit);
    std::set<std::filesystem::path> training_sources;
    for (const auto & source : candidate.at("sample_sources"))
        training_sources.insert(std::filesystem::weakly_canonical(input::resolve_path(candidate_path, source)));
    for (const auto & source : data.sources) {
        if (training_sources.contains(std::filesystem::path(source.get<std::string>())))
            throw std::invalid_argument("cost evaluation must not reuse a fitting sample file");
    }

    // Evaluate the fixed values directly. This path never calls fit or changes
    // the candidate; equal recorded contexts alone do not establish portability.
    nlohmann::json comparison;
    if (communication) comparison = CommunicationCostFitter{}.evaluate(candidate.at("entries"), data.communication_samples);
    else if (kernel) comparison = KernelCostFitter{}.evaluate(candidate.at("entries"), data.kernel_samples);
    else if (worker) comparison = WorkerCostFitter{}.evaluate(candidate.at("entries"), data.worker_samples);
    else comparison = AdmissionCostFitter{}.evaluate(candidate.at("entries"), data.samples);
    return {
        {                          "status", comparison.at("overall").at("count") == 0 ? "needs_data" : "evaluated" },
        {                   "qualification",                                                      "not_established" },
        {                       "candidate",  std::filesystem::absolute(candidate_path).lexically_normal().string() },
        {            "measurement_manifest",   std::filesystem::absolute(manifest_path).lexically_normal().string() },
        {           "sample_files_disjoint",                                                                   true },
        {          "observed_context_equal",                       candidate.at("observed_context") == data.context },
        {           "measurement_readiness",                                                         data.readiness },
        {                "measurement_gaps",                                                              data.gaps },
        { "candidate_measurement_readiness",                                  candidate.at("measurement_readiness") },
        {      "candidate_measurement_gaps",                                       candidate.at("measurement_gaps") },
        {                        "excluded",                                                          data.excluded },
        {                      "comparison",                                                             comparison }
    };
}

} // namespace markov
