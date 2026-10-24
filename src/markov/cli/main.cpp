#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/adapters/frameworks/sglang/structure.hpp"
#include "markov/contracts/topology.hpp"
#include "markov/controller/serving.hpp"
#include "markov/execution/durations.hpp"
#include "markov/graph/source_graph.hpp"
#include "markov/production/source_projection.hpp"
#include "markov/templates/instance.hpp"
#include "markov/templates/library.hpp"
#include "markov/templates/target_graph.hpp"
#include "markov/workflow/serving.hpp"

namespace {

nlohmann::json read_json(const std::filesystem::path & path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open " + path.string());

    nlohmann::json result;
    input >> result;
    return result;
}

void write_json(const std::filesystem::path & path, const nlohmann::json & result, const std::vector<std::filesystem::path> & inputs, bool readable = false) {
    const std::filesystem::path temporary = path.string() + ".tmp";
    const auto destination = std::filesystem::weakly_canonical(path);
    const auto staging = std::filesystem::weakly_canonical(temporary);

    // Publication and staging must both preserve declared inputs, including a hard-linked staging file.
    for (const auto & input : inputs) {
        const auto source = std::filesystem::weakly_canonical(input);
        if (destination == source || staging == source || (std::filesystem::exists(temporary) && std::filesystem::equivalent(temporary, input)))
            throw std::invalid_argument("output or staging path must not overwrite an input");
    }

    // Publish only after serialization succeeds, keeping the previous artifact intact on failure.
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(temporary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    if (readable) output << std::setw(2);
    output << result << '\n';
    output.close();
    std::filesystem::rename(temporary, path);
}

nlohmann::json compose(const markov::TemplateAsset & asset, const nlohmann::json & target) {
    markov::RegionGraph shared;
    shared.execution_resources = asset.execution_resources();
    markov::TargetGraph graph(asset, shared);
    std::vector<markov::TargetRegion> calls;
    const auto & invocations = target.at("invocations");
    if (!invocations.is_array() || invocations.empty()) throw std::invalid_argument("composition requires a nonempty invocation array");

    for (const auto & call : invocations) {
        std::map<std::string, std::size_t> inputs;
        if (call.contains("inputs")) {
            for (const auto & [name, source] : call.at("inputs").items()) {
                const auto & previous = calls.at(source.at("call").get<std::size_t>());
                inputs.emplace(name, previous.ports.outputs.at(source.at("port").get<std::string>()));
            }
        }

        calls.push_back(graph.append(call.at("role").get<std::string>(), call.at("phase").get<std::string>(), markov::Invocation::read(call), inputs));
    }

    return graph.document();
}

std::int64_t integer(const nlohmann::json & source) {
    if (!source.is_number_integer()) throw std::invalid_argument("serving fields must be integers");
    return source.get<std::int64_t>();
}

void check_fields(const nlohmann::json & source, std::initializer_list<std::string_view> fields, std::string_view context) {
    for (const auto & [name, _] : source.items()) {
        if (std::ranges::find(fields, std::string_view(name)) == fields.end())
            throw std::invalid_argument(std::string(context) + " has unsupported field: " + name);
    }
}

markov::ParallelTopology tensor_parallel_topology(const nlohmann::json & configuration, std::size_t rank_count) {
    const nlohmann::json * devices = nullptr;
    if (configuration.contains("placement")) {
        const auto & placement = configuration.at("placement");
        check_fields(placement, { "devices" }, "placement");
        devices = &placement.at("devices");
        if (!devices->is_array() || devices->size() != rank_count) throw std::invalid_argument("placement needs one device per TP rank");
    }

    std::vector<markov::RankPlacement> ranks;
    std::vector<std::size_t> members;
    std::set<std::size_t> used;
    for (std::size_t rank = 0; rank < rank_count; ++rank) {
        const auto device = devices == nullptr ? static_cast<std::int64_t>(rank) : integer(devices->at(rank));
        if (device < 0 || !used.insert(static_cast<std::size_t>(device)).second)
            throw std::invalid_argument("TP placement needs distinct nonnegative devices on one host");

        ranks.push_back({ "local", static_cast<std::size_t>(device) });
        members.push_back(rank);
    }
    return { std::move(ranks), { { "model", "tp", std::move(members) } } };
}

nlohmann::json simulate(std::span<const markov::TemplateAsset> assets, const nlohmann::json & input, bool details) {
    check_fields(input, { "configuration", "requests", "node_durations_ns" }, "serving input");
    const auto & configuration = input.at("configuration");
    check_fields(configuration, { "max_running_requests", "max_total_tokens", "page_size", "max_prefill_tokens", "placement" }, "configuration");
    const markov::ServingSettings settings{ integer(configuration.at("max_running_requests")),
                                            integer(configuration.at("max_total_tokens")),
                                            integer(configuration.at("page_size")),
                                            integer(configuration.at("max_prefill_tokens")) };

    std::vector<markov::Request> requests;
    if (!input.at("requests").is_array() || input.at("requests").empty()) throw std::invalid_argument("serving needs a nonempty request array");
    for (const auto & request : input.at("requests")) {
        check_fields(request, { "id", "arrival_ns", "prompt_tokens", "output_tokens" }, "request");
        requests.push_back({ request.at("id").get<std::string>(),
                             integer(request.at("arrival_ns")),
                             integer(request.at("prompt_tokens")),
                             integer(request.at("output_tokens")) });
    }

    const markov::GivenDurations durations(input.at("node_durations_ns"));
    const auto topology = tensor_parallel_topology(configuration, assets.size());
    const auto control_group = topology.groups().front().id;
    markov::sglang::ServingStructure expansion(assets, settings, requests, topology);
    markov::ServingController controller(settings, std::move(requests), control_group);
    markov::ServingWorkflow workflow(controller, expansion, durations);
    auto result = workflow.run(details);
    result["configuration"] = configuration;
    result["duration_source"] = "given_execution_kind_ns";
    result["structure"] = expansion.summary();
    if (details) result["graph"] = expansion.document();
    return result;
}

} // namespace

int main(int argc, char ** argv) {
    try {
        if (argc >= 2 && std::string(argv[1]) == "simulate") {
            std::vector<std::filesystem::path> paths;
            std::filesystem::path input;
            std::filesystem::path output;
            bool details = false;
            for (int index = 2; index < argc; ++index) {
                const std::string option = argv[index];
                if (option == "--details" && !details) {
                    details = true;
                    continue;
                }
                if (index + 1 == argc) throw std::invalid_argument("simulate option has no value");
                const std::string value = argv[++index];
                if (option == "--templates") paths.emplace_back(value);
                else if (option == "--input" && input.empty()) input = value;
                else if (option == "--output" && output.empty()) output = value;
                else throw std::invalid_argument("unknown or repeated simulate option: " + option);
            }
            if (paths.empty() || input.empty() || output.empty()) throw std::invalid_argument("simulate requires templates, input and output");
            std::vector<markov::TemplateAsset> assets;
            for (const auto & path : paths) assets.emplace_back(read_json(path));
            const auto result = simulate(assets, read_json(input), details);
            paths.push_back(input);
            write_json(output, result, paths, !details);
            std::cout << nlohmann::json{
                {          "scope",                                result.at("scope") },
                {         "status",                               result.at("status") },
                { "control_status",                       result.at("control_status") },
                {        "batches", result.at("structure").at("counts").at("batches") },
                {       "requests",                      result.at("requests").size() },
                {     "free_pages",                           result.at("free_pages") }
            }.dump(2) << '\n';
            return 0;
        }

        const bool inspect = argc == 4 && std::string(argv[1]) == "inspect" && std::string(argv[2]) == "--source";
        const bool with_physical = argc == 8 && std::string(argv[4]) == "--physical" && std::string(argv[6]) == "--output";
        const bool source_output = (argc == 6 && std::string(argv[4]) == "--output") || with_physical;
        const bool produce = source_output && std::string(argv[1]) == "produce" && std::string(argv[2]) == "--source";
        const bool templates = source_output && std::string(argv[1]) == "templates" && std::string(argv[2]) == "--source";
        const bool expand = argc == 8 && std::string(argv[1]) == "expand" && std::string(argv[2]) == "--templates" && std::string(argv[4]) == "--target"
                            && std::string(argv[6]) == "--output";
        const bool composition = argc == 8 && std::string(argv[1]) == "compose" && std::string(argv[2]) == "--templates" && std::string(argv[4]) == "--target"
                                 && std::string(argv[6]) == "--output";
        if (!inspect && !produce && !templates && !expand && !composition) {
            std::cerr << "Usage: markov inspect --source observations.json\n"
                         "       markov produce --source observations.json [--physical physical.json] --output source-graph.json\n"
                         "       markov templates --source observations.json [--physical physical.json] --output templates.json\n"
                         "       markov expand --templates templates.json --target target.json --output region.json\n"
                         "       markov compose --templates templates.json --target calls.json --output graph.json\n"
                         "       markov simulate --templates rank.json [--templates rank.json ...] --input serving.json --output result.json [--details]\n";
            return 2;
        }

        if (composition) {
            const markov::TemplateAsset asset(read_json(argv[3]));
            const auto target = read_json(argv[5]);
            const auto result = compose(asset, target);
            write_json(argv[7], result, { argv[3], argv[5] });
            std::cout << nlohmann::json{
                {              "scope",               result.at("scope") },
                {        "invocations",  target.at("invocations").size() },
                {         "operations",   result.at("operations").size() },
                {             "values",       result.at("values").size() },
                {          "resources",           result.at("resources") },
                { "external_resources",  result.at("external_resources") },
                {       "dependencies", result.at("dependencies").size() }
            }.dump(2) << '\n';
            return 0;
        }

        if (expand) {
            const markov::TemplateAsset asset(read_json(argv[3]));
            const auto target = read_json(argv[5]);
            auto instance = markov::TemplateInstance::select(asset,
                                                             target.at("role").get<std::string>(),
                                                             target.at("phase").get<std::string>(),
                                                             markov::Invocation::read(target));
            const auto result = instance.document();
            write_json(argv[7], result, { argv[3], argv[5] });
            std::cout << nlohmann::json{
                {                    "scope",                    result.at("scope") },
                {                     "role",                     result.at("role") },
                {            "state_changes",       instance.state_changes().size() },
                {              "input_ports",        instance.ports().inputs.size() },
                {             "output_ports",       instance.ports().outputs.size() },
                { "source_physical_coverage", result.at("source_physical_coverage") },
                {  "source_runtime_coverage",  result.at("source_runtime_coverage") },
                { "source_runtime_arguments", result.at("source_runtime_arguments") },
                {                   "layout",                   result.at("layout") },
                {               "operations",        result.at("operations").size() },
                {                   "values",            result.at("values").size() },
                {             "dependencies",      result.at("dependencies").size() }
            }.dump(2) << '\n';
            return 0;
        }

        // Source commands share contract inspection before projection or template construction.
        const markov::SourceGraph graph(read_json(argv[3]));
        const auto summary = graph.summary();
        const auto & gaps = summary.at("contract_gaps");
        if (inspect) {
            std::cout << summary.dump(2) << '\n';
            return gaps.empty() ? 0 : 1;
        }
        if (!gaps.empty()) throw std::invalid_argument("source contract has " + std::to_string(gaps.size()) + " gaps; first: " + gaps.front().dump());

        const auto physical = with_physical ? read_json(argv[5]) : nlohmann::json(nullptr);
        auto result = markov::SourceProjection(graph).document(with_physical ? &physical : nullptr);
        if (templates) result = markov::TemplateLibrary(result).document();

        if (with_physical) write_json(argv[7], result, { argv[3], argv[5] });
        else write_json(argv[5], result, { argv[3] });

        auto report = templates ? nlohmann::json{ { "definitions", result.at("definitions").size() }, { "layouts", result.at("layouts").size() }, { "entries", result.at("entries").size() } }
                                      : nlohmann::json{ { "operations", result.at("operations").size() }, { "values", result.at("values").size() }, { "dependencies", result.at("dependencies").size() } };
        report["physical_coverage"] = result.at("physical_coverage").at("status");
        if (templates) report["execution_variants"] = result.at("execution_variants").size();
        report["runtime_coverage"] = result.at("runtime_coverage").at("status");
        if (with_physical) report["unowned_work_kinds"] = result.at("physical_coverage").at("unowned").size();
        std::cout << report.dump(2) << '\n';
        return 0;
    }
    catch (const std::exception & error) {
        std::cerr << "markov: " << error.what() << '\n';
        return 1;
    }
}
