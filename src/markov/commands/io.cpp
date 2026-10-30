#include "markov/commands/io.hpp"

#include <fstream>
#include <stdexcept>
#include <system_error>

namespace markov::commands {

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

} // namespace markov::commands
