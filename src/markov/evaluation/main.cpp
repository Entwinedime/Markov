#include "markov/commands/io.hpp"
#include "markov/evaluation/evaluation.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char ** argv) {
    if (argc != 4 || std::string_view(argv[1]) != "evaluate") {
        std::cerr << "usage: markov_evaluate evaluate <evaluation_request.json> <report.json>\n";
        return 2;
    }
    try {
        const auto input = markov::evaluation::prepare(argv[2]);
        std::vector<std::filesystem::path> protected_inputs{ std::filesystem::absolute(argv[2]),
                                                             input.prediction.prediction,
                                                             input.prediction.source_manifest,
                                                             input.target.manifest.path };
        for (const auto & file : input.target.manifest.files) protected_inputs.push_back(file.path);
        for (const auto & file : input.base.files) protected_inputs.push_back(file.path);
        if (input.base.configuration_path) protected_inputs.push_back(*input.base.configuration_path);
        if (input.target.manifest.configuration_path) protected_inputs.push_back(*input.target.manifest.configuration_path);
        const auto output = std::filesystem::absolute(argv[3]).lexically_normal();
        markov::commands::prepare_output(output, protected_inputs);
        markov::commands::write_report(markov::evaluation::report(input), output);
        return 0;
    }
    catch (const std::exception & error) {
        std::cerr << "markov_evaluate: " << error.what() << '\n';
        return 2;
    }
}
