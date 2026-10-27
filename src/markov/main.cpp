#include "markov/trace_graph/source/manifest.hpp"

#include <exception>
#include <iostream>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

int main(int argc, char ** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "usage: markov_core inspect-manifest <profile_manifest.json>\n";
        return 0;
    }

    if (argc != 3 || std::string_view(argv[1]) != "inspect-manifest") {
        std::cerr << "usage: markov_core inspect-manifest <profile_manifest.json>\n";
        return 2;
    }

    try {
        const auto manifest = markov::trace_graph::source::inspect_manifest(argv[2]);
        auto files = nlohmann::json::array();
        for (const auto & file : manifest.files)
            files.push_back({
                { "channel", channel_name(file.channel) },
                {    "path",         file.path.string() },
                {   "bytes",                 file.bytes }
            });

        const nlohmann::json result{
            {                "status",    "inputs_accessible" },
            {              "manifest", manifest.path.string() },
            {             "framework",     manifest.framework },
            {                 "files",       std::move(files) },
            { "trace_contents_parsed",                  false },
            {             "dag_built",                  false }
        };
        std::cout << result.dump(2) << '\n';
        return 0;
    }
    catch (const std::exception & error) {
        std::cerr << "markov_core: " << error.what() << '\n';
        return 2;
    }
}
