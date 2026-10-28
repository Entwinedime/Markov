#include "markov/adapters/costs.hpp"
#include "markov/adapters/source.hpp"
#include "markov/commands/costs.hpp"
#include "markov/commands/source.hpp"
#include "markov/trace_graph/source/manifest.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

void usage(std::ostream & stream) {
    stream << "usage: markov_core inspect-manifest <profile_manifest.json> [report.json]\n"
              "       markov_core source <profile_manifest.json> [report.json]\n"
              "       markov_core page-work <page_work_request.json> [report.json]\n"
              "       markov_core costs <cost_queries.json> [report.json]\n";
}

nlohmann::json manifest_report(const markov::trace_graph::source::ProfileManifest & manifest) {
    auto files = nlohmann::json::array();
    for (const auto & file : manifest.files)
        files.push_back({
            { "channel", channel_name(file.channel) },
            {    "path",         file.path.string() },
            {   "bytes",                 file.bytes }
        });

    return {
        {                "status",    "inputs_accessible" },
        {              "manifest", manifest.path.string() },
        {             "framework",     manifest.framework },
        {                 "files",       std::move(files) },
        { "trace_contents_parsed",                  false },
        {             "dag_built",                  false }
    };
}

void write_report(const nlohmann::json & result, const std::filesystem::path & destination) {
    auto pending = destination;
    pending += ".partial";
    std::ofstream stream(pending, std::ios::out | std::ios::noreplace);
    if (!stream) throw std::runtime_error("Cannot write report: " + destination.string());
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    try {
        stream << result.dump(2) << '\n';
        stream.close();
        std::filesystem::rename(pending, destination);
    }
    catch (...) {
        std::error_code ignored;
        std::filesystem::remove(pending, ignored);
        throw;
    }
}

void prepare_output(const std::optional<std::filesystem::path> & output, const std::vector<std::filesystem::path> & inputs) {
    if (!output) return;

    auto pending = *output;
    pending += ".partial";
    for (const auto & input : inputs) {
        if (*output == input || pending == input) throw std::runtime_error("Report output would replace an input: " + input.string());
    }
    std::filesystem::create_directories(output->parent_path());
}

} // namespace

int main(int argc, char ** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        usage(std::cout);
        return 0;
    }
    if (argc < 3 || argc > 4) {
        usage(std::cerr);
        return 2;
    }
    const std::string_view command(argv[1]);
    if (command != "inspect-manifest" && command != "source" && command != "costs" && command != "page-work") {
        usage(std::cerr);
        return 2;
    }

    try {
        std::optional<std::filesystem::path> output;
        if (argc == 4) output = std::filesystem::absolute(argv[3]).lexically_normal();

        nlohmann::json result;
        if (command == "costs") {
            const auto queries = markov::adapters::read_cost_queries(argv[2]);
            prepare_output(output, { std::filesystem::absolute(argv[2]).lexically_normal(), queries.models_path });
            const auto selected = markov::adapters::read_cost_models(queries.models_path);
            result = markov::commands::cost_report(queries, selected);
        }
        else {
            const auto page_request = command == "page-work" ? std::optional(markov::adapters::read_page_work_request(argv[2])) : std::nullopt;
            const auto selected_costs =
                page_request && page_request->models_path ? std::optional(markov::adapters::read_cost_models(*page_request->models_path)) : std::nullopt;
            const auto manifest = page_request ? page_request->manifest : markov::trace_graph::source::inspect_manifest(argv[2]);
            if (selected_costs && (selected_costs->deployment.at("framework") != manifest.framework || selected_costs->deployment.at("runtime") != "ascend"))
                throw std::runtime_error("Page-work source and selected cost framework/runtime differ");
            std::vector<std::filesystem::path> inputs{ manifest.path };
            if (page_request) inputs.push_back(std::filesystem::absolute(argv[2]).lexically_normal());
            if (selected_costs) inputs.push_back(*page_request->models_path);
            if (manifest.capture_origin_manifest) inputs.push_back(*manifest.capture_origin_manifest);
            for (const auto & file : manifest.files) inputs.push_back(file.path);
            prepare_output(output, inputs);
            if (page_request)
                result = markov::commands::page_work_report(markov::adapters::prepare_source(manifest),
                                                            page_request->page_sizes_tokens,
                                                            selected_costs ? &*selected_costs : nullptr);
            else result = command == "source" ? markov::commands::source_report(markov::adapters::prepare_source(manifest)) : manifest_report(manifest);
            if (selected_costs) result["models"] = page_request->models_path->string();
        }
        if (output) write_report(result, *output);
        else std::cout << result.dump(2) << '\n';
        return 0;
    }
    catch (const std::exception & error) {
        std::cerr << "markov_core: " << error.what() << '\n';
        return 2;
    }
}
