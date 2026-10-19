#include "markov/workflow/cost_fitting_workflow.hpp"

#include "markov/contracts/json_input.hpp"
#include "markov/performance/communication_cost_model.hpp"
#include "markov/performance/cpu_cost_model.hpp"
#include "markov/performance/device_command_cost_model.hpp"
#include "markov/performance/kernel_cost_model.hpp"
#include "markov/performance/measured_cost_fitter.hpp"
#include "markov/performance/transfer_cost_model.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace markov {

struct CostFittingWorkflow::Observation {
    std::filesystem::path manifest_path;
    std::filesystem::path measurements_path;
    nlohmann::json manifest;
    nlohmann::json context;
    nlohmann::json measurements;
    std::filesystem::path cpu_path;
    nlohmann::json cpu_samples;
    nlohmann::json readiness;
    nlohmann::json gaps;
    std::set<std::filesystem::path> device_sources;
};

struct CostFittingWorkflow::Measurements {
    std::map<std::string, std::size_t> excluded;
    nlohmann::json sources = nlohmann::json::array();
    MeasuredCostFitter<KernelCostFeatures> kernels;
    MeasuredCostFitter<CommunicationCostFeatures> communication;
    MeasuredCostFitter<TransferCostFeatures> transfers;
    MeasuredCostFitter<DeviceCommandWork> commands;
    MeasuredCostFitter<CpuServiceWork> cpu;
};

CostFittingWorkflow::Observation CostFittingWorkflow::read(const std::filesystem::path & manifest_path) const {
    Observation source;
    source.manifest_path = manifest_path;
    source.manifest = input::read_json(manifest_path);
    const auto & manifest = source.manifest;
    const auto & kind = manifest.at("measurement_kind");
    if (manifest.at("role") != "performance"
        || (kind != "native_serving_timing" && kind != "native_cpu_activity" && kind != "native_device_work" && kind != "native_collective_work"))
        throw std::invalid_argument("cost fitting requires independent performance measurements");

    const auto prepared = input::read_json(input::resolve_path(manifest_path, manifest.at("input")));
    const auto environment = input::read_json(input::resolve_path(manifest_path, manifest.at("environment")));
    if (kind != "native_collective_work" && environment.at("steady_state").at("status") != "steady")
        throw std::invalid_argument("serving cost fitting requires observed steady measurement windows");

    source.context = {
        {         "framework",         environment.at("framework") },
        { "framework_version", environment.at("framework_version") },
        {        "model_path",           prepared.at("model_path") },
        {     "configuration",        prepared.at("configuration") },
        {        "deployment",          prepared.at("environment") },
        {           "runtime",           environment.at("runtime") }
    };
    source.gaps = manifest.at("gaps");
    const bool host_transfer = kind == "native_serving_timing" || kind == "native_cpu_activity";
    std::string_view field = "device_measurements";
    if (host_transfer) field = "cpu_worker_units";
    else if (kind == "native_collective_work") field = "collective_measurements";
    source.measurements_path = input::resolve_path(manifest_path, manifest.at(field));
    // A device capture serves four cost classes. Decode it once; each class
    // retains its own sample filtering, exclusions and provenance below.
    source.measurements = input::read_json(source.measurements_path);

    if (host_transfer) {
        if (manifest.contains("cpu_service_units")) {
            source.cpu_path = input::resolve_path(manifest_path, manifest.at("cpu_service_units"));
            source.cpu_samples = input::read_json(source.cpu_path);
        }
        auto workers = nlohmann::json::array();
        for (const auto & reference : manifest.at("worker_environments")) {
            const auto worker = input::read_json(input::resolve_path(manifest_path, reference));
            workers.push_back({
                {    "rank",    worker.at("rank") },
                { "runtime", worker.at("runtime") }
            });
        }
        source.context["workers"] = std::move(workers);
        source.readiness = manifest.at("cpu_worker_readiness");
        const auto & gaps = manifest.at("cpu_worker_gaps");
        source.gaps.insert(source.gaps.end(), gaps.begin(), gaps.end());
        return source;
    }

    source.readiness = manifest.at("readiness");
    source.device_sources.insert(source.measurements_path);
    // Journal locators preserve capture identity after raw payload retention ends.
    const auto capture_path = input::resolve_path(manifest_path, manifest.at("native_capture"));
    const auto capture = input::read_json(capture_path);
    for (const auto & reference : capture.at("observations")) source.device_sources.insert(input::resolve_path(capture_path, reference));

    auto devices = source.measurements.at("devices");
    for (auto & device : devices) {
        device.erase("pid");
        device.erase("source");
    }
    source.context["devices"] = std::move(devices);
    return source;
}

CostFittingWorkflow::Measurements CostFittingWorkflow::select(const Observation & source, std::string_view unit) const {
    if (unit != "kernel" && unit != "communication" && unit != "transfer" && unit != "command" && unit != "cpu")
        throw std::invalid_argument("unknown cost unit: " + std::string(unit));
    const auto & kind = source.manifest.at("measurement_kind");
    const bool host = kind == "native_serving_timing" || kind == "native_cpu_activity";
    if ((host && unit != "transfer" && unit != "cpu") || (kind == "native_collective_work" && unit != "communication"))
        throw std::invalid_argument("cost fitting requires independent measurements for the selected unit");

    Measurements data;
    if (unit == "cpu") read_cpu(data, source);
    else if (host) read_host_transfers(data, source);
    else read_device(data, source, unit);
    return data;
}

void CostFittingWorkflow::read_cpu(Measurements & data, const Observation & source) const {
    const auto & observations = source.cpu_samples;
    if (observations.at("kind") != "cpu_service_samples" || observations.at("unit") != "ns")
        throw std::invalid_argument("CPU fitting requires named exclusive service intervals in ns");

    data.sources.push_back(std::filesystem::weakly_canonical(source.cpu_path).string());
    for (const auto field : { "cpu_activities", "transfer_scopes" })
        for (const auto & reference : source.manifest.at(field))
            data.sources.push_back(std::filesystem::weakly_canonical(input::resolve_path(source.manifest_path, reference)).string());

    // Native layers and repetitions reference one work definition. Resolve it
    // once before accumulating durations into the common measured groups.
    std::vector<CpuServiceWork> features;
    for (const auto & value : observations.at("features")) features.push_back(CpuServiceWork::read(value));
    for (const auto & sample : observations.at("samples"))
        data.cpu.observe(features.at(sample.at("feature").get<std::size_t>()),
                         input::integer(sample.at("duration_ns"), 0, std::numeric_limits<std::int64_t>::max()));
}

void CostFittingWorkflow::read_host_transfers(Measurements & data, const Observation & source) const {
    const auto & observations = source.measurements;
    if (observations.at("kind") != "cpu_worker_units" || observations.at("unit") != "ns")
        throw std::invalid_argument("host transfer fitting requires normalized CPU worker units in ns");

    data.sources.push_back(std::filesystem::weakly_canonical(source.measurements_path).string());

    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    for (const auto & record : observations.at("transfer_units")) {
        if (record.at("stage") == "warmup") {
            ++data.excluded["warmup"];
            continue;
        }
        if (record.at("stage") != "measurement") throw std::invalid_argument("unknown host transfer measurement stage");

        const auto & boundaries = record.at("boundaries");
        if (record.at("result") != 0 || boundaries.size() != 2) {
            ++data.excluded["complete_successful_host_transfer_not_observed"];
            continue;
        }
        const auto begin = input::integer(boundaries.front().at("wall_ns"), 0, maximum);
        const auto end = input::integer(boundaries.back().at("wall_ns"), 0, maximum);
        const auto duration = input::integer(record.at("duration_ns"), 0, maximum);
        if (end < begin || duration != end - begin) throw std::invalid_argument("host transfer duration disagrees with its complete driver boundaries");

        const auto features = TransferCostFeatures::read(record.at("transfer"));
        if (features.execution != TransferExecution::SynchronousHostDriver)
            throw std::invalid_argument("host transfer measurements require the synchronous driver boundary");

        // The complete driver call includes its I/O stalls. Thread CPU and
        // probe counters cannot be subtracted from that wall completion time.
        data.transfers.observe(features, duration);
    }
}

void CostFittingWorkflow::read_device(Measurements & data, const Observation & source, std::string_view unit) const {
    const auto & observations = source.measurements;
    const auto & path = source.measurements_path;
    auto sources = source.device_sources;

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

            data.communication.observe(CommunicationCostFeatures::read(sample),
                                       input::integer(sample.at("observed_wall_ns"), 0, std::numeric_limits<std::int64_t>::max()));
            continue;
        }
        if (unit == "command") {
            if (sample.at("kind") != "device_marker" || sample.at("device_command").is_null()) {
                ++data.excluded["not_observed_device_command"];
                continue;
            }

            data.commands.observe(DeviceCommandWork::read(sample.at("device_command")),
                                  input::integer(sample.at("duration_ns"), 0, std::numeric_limits<std::int64_t>::max()));
            continue;
        }
        if (unit == "transfer") {
            if (sample.at("kind") != "dma") {
                ++data.excluded["not_transfer"];
                continue;
            }
            if (sample.at("transfer").is_null()) {
                ++data.excluded["transfer_work_not_observed"];
                continue;
            }

            // Existing device observations store source work. Their physical
            // DMA role supplies the execution boundary of the fitted cost key.
            auto transfer = sample.at("transfer");
            transfer["execution"] = "asynchronous_device";
            data.transfers.observe(TransferCostFeatures::read(transfer), input::integer(sample.at("duration_ns"), 0, std::numeric_limits<std::int64_t>::max()));
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

        data.kernels.observe(KernelCostFeatures::read(sample.at("kernel_work")),
                             input::integer(sample.at("duration_ns"), 0, std::numeric_limits<std::int64_t>::max()));
    }
    for (const auto & source : sources) data.sources.push_back(std::filesystem::weakly_canonical(source).string());
}

nlohmann::json CostFittingWorkflow::run(const std::filesystem::path & manifest_path, std::string_view unit) const {
    const auto source = read(manifest_path);
    if (unit != "all") return fit(source, unit);

    std::vector<std::string_view> units;
    const auto & manifest = source.manifest;
    const auto & kind = manifest.at("measurement_kind");
    if (kind == "native_serving_timing" || kind == "native_cpu_activity") {
        if (manifest.at("cpu_worker_coverage").contains("transfer_units")) units.push_back("transfer");
        if (manifest.contains("cpu_service_units")) units.push_back("cpu");
    }
    else if (kind == "native_device_work") units = { "kernel", "communication", "transfer", "command" };
    else units = { "communication" };

    auto candidates = nlohmann::json::array();
    for (const auto selected : units)
        candidates.push_back({
            { "cost_unit", selected },
            { "candidate", fit(source, selected) }
        });
    return {
        { "source_manifest", std::filesystem::absolute(manifest_path).lexically_normal().string() },
        {      "candidates",                                                std::move(candidates) }
    };
}

nlohmann::json CostFittingWorkflow::fit(const Observation & source, std::string_view unit) const {
    const auto data = select(source, unit);
    nlohmann::json entries;
    std::string_view kind;
    std::vector<std::string> assumptions;
    if (unit == "cpu") {
        entries = data.cpu.fit();
        kind = "empirical_cpu_service_median";
        assumptions = { "normalized thread CPU between observed named entries; marked observer work and directly observed waits/transfers excluded",
                        "exact shared service descriptor and resolved target work; no interpolation or extrapolation",
                        "native public runtime entries are associated with CANN implementation ranges by the runtime adapter",
                        "opaque SDK readiness CPU, including any polling, overlaps its retained device dependency; internal CPU placement is not reconstructed",
                        "median rounded down to integral ns; repeated layers remain one native run, not independent validation",
                        "descheduling and unobserved external contention excluded; recorded deployment only; independent accuracy not established" };
    }
    else if (unit == "command") {
        entries = data.commands.fit();
        kind = "empirical_device_command_median";
        assumptions = { "physical device record command span only; CPU submission and dependent waits remain separate",
                        "exact adapter-provided task type; event identity, stream and rank omitted",
                        "median rounded down to integral ns; no outlier removal, interpolation or extrapolation",
                        "profiler resolution, overhead and internal stalls not removed; recorded deployment only; service transfer not established" };
    }
    else if (unit == "transfer") {
        entries = data.transfers.fit();
        kind = "empirical_transfer_median";
        const bool host_transfer =
            source.manifest.at("measurement_kind") == "native_serving_timing" || source.manifest.at("measurement_kind") == "native_cpu_activity";
        assumptions = { host_transfer
                            ? "complete synchronous host driver copy wall interval, including its I/O stalls; preceding runtime wait and CPU setup excluded"
                            : "physical asynchronous device memory-copy span only; CPU submission and waits excluded",
                        "exact execution boundary, bytes and semantic direction; no interpolation or extrapolation",
                        "median rounded down to integral ns; no outlier removal",
                        host_transfer ? "driver observation clock/probe overhead and contention not removed; thread CPU is not a wall-time correction"
                                      : "profiler overhead, internal stalls and contention not removed",
                        "rank and host-memory properties omitted; recorded deployment only; service transfer not established" };
    }
    else if (unit == "communication") {
        entries = data.communication.fit();
        kind = "empirical_communication_median";
        assumptions = { "physical data and command tasks only; waits and collective envelopes excluded",
                        "physical device span; profiler overhead and internal transport stalls not removed",
                        "median rounded down to integral ns; no outlier removal",
                        "exact task kind, operation, bytes, dtype, transport and rank endpoints; no interpolation or extrapolation",
                        "recorded deployment only; concurrent contention and runtime transfer not established" };
    }
    else if (unit == "kernel") {
        entries = data.kernels.fit();
        kind = "empirical_kernel_median";
        assumptions = { "physical device span; profiler overhead and hidden stalls not removed",
                        "median rounded down to integral ns; no outlier removal",
                        "exact kernel, tensor, launch, tensor-traversal policy and observed attention features; no interpolation or extrapolation",
                        "producer-declared fixed tensor traversals may omit semantic operand values; this is a conditional cost assumption",
                        "unmodeled loops/indexed accesses, rank, phase and cache state remain unqualified; independent validation required",
                        "recorded deployment only; runtime equivalence and target launch mapping not established" };
    }

    // Fitting never upgrades measurement readiness or absorbs waits into cost.
    return {
        {                  "kind",                                                                           kind },
        {                  "unit",                                                                           "ns" },
        {                "status",                                   entries.empty() ? "needs_data" : "candidate" },
        {       "source_manifest",    std::filesystem::absolute(source.manifest_path).lexically_normal().string() },
        {        "sample_sources",                                                                   data.sources },
        {      "observed_context",                                                                 source.context },
        { "measurement_readiness", unit == "cpu" ? source.manifest.at("cpu_service_readiness") : source.readiness },
        {      "measurement_gaps",                                                                    source.gaps },
        {               "entries",                                                                        entries },
        {              "excluded",                                                                  data.excluded },
        {           "assumptions",                                                                    assumptions }
    };
}

nlohmann::json CostFittingWorkflow::evaluate(const std::filesystem::path & candidate_path, const std::filesystem::path & manifest_path) const {
    const auto candidate = input::read_json(candidate_path);
    const auto & kind = candidate.at("kind");
    std::string_view unit;
    if (kind == "empirical_kernel_median") unit = "kernel";
    else if (kind == "empirical_communication_median") unit = "communication";
    else if (kind == "empirical_transfer_median") unit = "transfer";
    else if (kind == "empirical_device_command_median") unit = "command";
    else if (kind == "empirical_cpu_service_median") unit = "cpu";
    else throw std::invalid_argument("unknown cost model kind");
    if (candidate.at("unit") != "ns" || candidate.at("status") != "candidate")
        throw std::invalid_argument("cost evaluation requires a measured median candidate in ns");

    const auto source = read(manifest_path);
    const auto data = select(source, unit);
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
    if (unit == "cpu") comparison = data.cpu.evaluate(CpuCostModel(candidate.at("entries")));
    else if (unit == "command") comparison = data.commands.evaluate(DeviceCommandCostModel(candidate.at("entries")));
    else if (unit == "transfer") comparison = data.transfers.evaluate(TransferCostModel(candidate.at("entries")));
    else if (unit == "communication") comparison = data.communication.evaluate(CommunicationCostModel(candidate.at("entries")));
    else comparison = data.kernels.evaluate(KernelCostModel(candidate.at("entries")));
    return {
        {                          "status", comparison.at("overall").at("count") == 0 ? "needs_data" : "evaluated" },
        {                   "qualification",                                                      "not_established" },
        {                       "candidate",  std::filesystem::absolute(candidate_path).lexically_normal().string() },
        {            "measurement_manifest",   std::filesystem::absolute(manifest_path).lexically_normal().string() },
        {           "sample_files_disjoint",                                                                   true },
        {          "observed_context_equal",                     candidate.at("observed_context") == source.context },
        {           "measurement_readiness",                                                       source.readiness },
        {                "measurement_gaps",                                                            source.gaps },
        { "candidate_measurement_readiness",                                  candidate.at("measurement_readiness") },
        {      "candidate_measurement_gaps",                                       candidate.at("measurement_gaps") },
        {                        "excluded",                                                          data.excluded },
        {                      "comparison",                                                             comparison }
    };
}

} // namespace markov
