#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/adapters/frameworks/sglang/batch_record.hpp"
#include "markov/application/control_stream.hpp"
#include "markov/application/simulation.hpp"
#include "markov/controller/batch_replay.hpp"
#include "markov/graph/source_graph.hpp"
#include "markov/production/source_projection.hpp"
#include "markov/production/template_library.hpp"
#include "markov/templates/instance.hpp"
#include "markov/templates/target_graph.hpp"

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

const markov::TemplateAsset & select_role(std::span<const markov::TemplateAsset> roles, const nlohmann::json & target) {
    if (target.contains("source_rank") && !target.at("source_rank").is_number_integer()) throw std::invalid_argument("source_rank must be an integer");
    const auto rank = target.value("source_rank", std::int64_t{ 0 });
    const auto selected = std::ranges::find_if(roles, [rank](const auto & role) { return role.environment().at("rank") == rank; });
    if (selected == roles.end()) throw std::invalid_argument("template catalog has no selected source rank");
    return *selected;
}

} // namespace

int main(int argc, char ** argv) {
    try {
        if (argc >= 2 && std::string(argv[1]) == "record-batches") {
            std::vector<std::filesystem::path> sources;
            std::filesystem::path input;
            std::filesystem::path output;
            for (int index = 2; index < argc; ++index) {
                const std::string option = argv[index];
                if (index + 1 == argc) throw std::invalid_argument("record-batches option has no value");
                const std::filesystem::path value = argv[++index];
                if (option == "--source") sources.push_back(value);
                else if (option == "--input" && input.empty()) input = value;
                else if (option == "--output" && output.empty()) output = value;
                else throw std::invalid_argument("unknown or repeated record-batches option: " + option);
            }
            if (sources.empty() || input.empty() || output.empty()) throw std::invalid_argument("record-batches requires source, input and output");
            const auto specification = read_json(input);
            markov::sglang::BatchRecord record(specification.at("requests"));
            std::vector<std::filesystem::path> inputs{ input };
            for (const auto & source : sources) {
                const auto session = source.parent_path() / "session.json";
                record.append(read_json(source), read_json(session));
                inputs.insert(inputs.end(), { source, session });
            }
            const markov::BatchReplay checked(record.document(),
                                              markov::Request::read(specification.at("requests")),
                                              specification.at("configuration").contains("pd"));
            write_json(output, record.document(), inputs, true);
            std::cout << "native batch records written\n";
            return 0;
        }
        if (argc >= 2 && std::string(argv[1]) == "simulate") {
            std::filesystem::path template_path;
            std::filesystem::path input;
            std::filesystem::path output;
            std::filesystem::path batches;
            bool details = false;
            std::string control;
            for (int index = 2; index < argc; ++index) {
                const std::string option = argv[index];
                if (option == "--details" && !details) {
                    details = true;
                    continue;
                }
                if (index + 1 == argc) throw std::invalid_argument("simulate option has no value");
                const std::string value = argv[++index];
                if (option == "--templates" && template_path.empty()) template_path = value;
                else if (option == "--input" && input.empty()) input = value;
                else if (option == "--output" && output.empty()) output = value;
                else if (option == "--batches" && batches.empty()) batches = value;
                else if (option == "--control" && control.empty()) control = value;
                else throw std::invalid_argument("unknown or repeated simulate option: " + option);
            }
            if (template_path.empty() || input.empty() || output.empty()) throw std::invalid_argument("simulate requires templates, input and output");
            if (!control.empty() && control != "native") throw std::invalid_argument("unknown control implementation: " + control);
            auto document = read_json(template_path);
            const markov::TemplateCatalog catalog(std::move(document.at("catalog")));
            const auto assets = markov::TemplateAsset::read_roles(catalog, document.at("roles"));
            const auto specification = read_json(input);
            const auto recorded = batches.empty() ? nlohmann::json(nullptr) : read_json(batches);
            markov::ControlStream peer(std::cin, std::cout);
            markov::ServingSimulation simulation(assets, specification, recorded, control == "native" ? &peer : nullptr);
            const auto result = simulation.run(details);
            std::vector<std::filesystem::path> inputs{ template_path, input };
            if (!batches.empty()) inputs.push_back(batches);
            write_json(output, result, inputs, !details);
            (control == "native" ? std::cerr : std::cout) << nlohmann::json{
                {          "scope",                                result.at("scope") },
                {         "status",                               result.at("status") },
                { "control_status",                       result.at("control_status") },
                {        "batches", result.at("structure").at("counts").at("batches") },
                {        "actions", result.at("structure").at("counts").at("actions") },
                {       "requests",                      result.at("requests").size() },
                {     "free_pages",                           result.at("free_pages") }
            }.dump(2) << '\n';
            return 0;
        }

        if (argc >= 2 && std::string(argv[1]) == "templates") {
            std::vector<std::pair<std::filesystem::path, std::filesystem::path>> sources;
            std::filesystem::path output;
            std::filesystem::path instances;
            for (int index = 2; index < argc; ++index) {
                const std::string option = argv[index];
                if (index + 1 == argc) throw std::invalid_argument("templates option has no value");
                const std::filesystem::path value = argv[++index];
                if (option == "--source") sources.emplace_back(value, std::filesystem::path{});
                else if (option == "--physical" && !sources.empty() && sources.back().second.empty()) sources.back().second = value;
                else if (option == "--output" && output.empty()) output = value;
                else if (option == "--instances" && instances.empty()) instances = value;
                else throw std::invalid_argument("unknown or repeated templates option: " + option);
            }
            if (sources.empty() || output.empty()) throw std::invalid_argument("templates requires sources and output");

            markov::TemplateLibrary library;
            std::vector<std::filesystem::path> inputs;
            for (const auto & [source, physical_path] : sources) {
                const markov::SourceGraph graph(read_json(source));
                const auto gaps = graph.summary().at("contract_gaps");
                if (!gaps.empty()) throw std::invalid_argument("source contract has gaps in " + source.string() + ": " + gaps.front().dump());
                inputs.push_back(source);

                const auto physical = physical_path.empty() ? nlohmann::json(nullptr) : read_json(physical_path);
                library.add(markov::SourceProjection(graph).document(physical_path.empty() ? nullptr : &physical));
                if (!physical_path.empty()) inputs.push_back(physical_path);
            }
            if (!instances.empty()) {
                library.add_instances(read_json(instances).at("instances"));
                inputs.push_back(instances);
            }
            const auto result = std::move(library).document();
            const markov::TemplateCatalog catalog(result.at("catalog"));
            markov::TemplateAsset::read_roles(catalog, result.at("roles"));
            write_json(output, result, inputs);
            std::cout << nlohmann::json{
                {        "definitions",        result.at("catalog").at("definitions").size() },
                {            "layouts",            result.at("catalog").at("layouts").size() },
                { "execution_patterns", result.at("catalog").at("execution_patterns").size() },
                {              "roles",                            result.at("roles").size() }
            }.dump(2) << '\n';
            return 0;
        }

        const bool inspect = argc == 4 && std::string(argv[1]) == "inspect" && std::string(argv[2]) == "--source";
        const bool with_physical = argc == 8 && std::string(argv[4]) == "--physical" && std::string(argv[6]) == "--output";
        const bool source_output = (argc == 6 && std::string(argv[4]) == "--output") || with_physical;
        const bool produce = source_output && std::string(argv[1]) == "produce" && std::string(argv[2]) == "--source";
        const bool expand = argc == 8 && std::string(argv[1]) == "expand" && std::string(argv[2]) == "--templates" && std::string(argv[4]) == "--target"
                            && std::string(argv[6]) == "--output";
        const bool composition = argc == 8 && std::string(argv[1]) == "compose" && std::string(argv[2]) == "--templates" && std::string(argv[4]) == "--target"
                                 && std::string(argv[6]) == "--output";
        if (!inspect && !produce && !expand && !composition) {
            std::cerr << "Usage: markov inspect --source observations.json\n"
                         "       markov produce --source observations.json [--physical physical.json] --output source-graph.json\n"
                         "       markov templates --source observations.json [--physical physical.json] [--source ...] [--instances roles.json] --output "
                         "templates.json\n"
                         "       markov expand --templates templates.json --target target.json --output region.json\n"
                         "       markov compose --templates templates.json --target calls.json --output graph.json\n"
                         "       markov record-batches --source rank.json [--source peer.json] --input serving.json --output batches.json\n"
                         "       markov simulate --templates templates.json --input serving.json --output result.json [--batches batches.json | --control "
                         "native] [--details]\n";
            return 2;
        }

        if (composition) {
            auto document = read_json(argv[3]);
            const markov::TemplateCatalog catalog(std::move(document.at("catalog")));
            const auto roles = markov::TemplateAsset::read_roles(catalog, document.at("roles"));
            const auto target = read_json(argv[5]);
            const auto & asset = select_role(roles, target);
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
            auto document = read_json(argv[3]);
            const markov::TemplateCatalog catalog(std::move(document.at("catalog")));
            const auto roles = markov::TemplateAsset::read_roles(catalog, document.at("roles"));
            const auto target = read_json(argv[5]);
            const auto & asset = select_role(roles, target);
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
        const auto result = markov::SourceProjection(graph).document(with_physical ? &physical : nullptr);

        if (with_physical) write_json(argv[7], result, { argv[3], argv[5] });
        else write_json(argv[5], result, { argv[3] });

        auto report = nlohmann::json{
            {   "operations",   result.at("operations").size() },
            {       "values",       result.at("values").size() },
            { "dependencies", result.at("dependencies").size() }
        };
        report["physical_coverage"] = result.at("physical_coverage").at("status");
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
