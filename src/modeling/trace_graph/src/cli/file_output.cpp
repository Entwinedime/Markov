/**
 * @file
 * @brief Implements checked JSON and text artifact output.
 */
#include "file_output.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace markov::trace_graph::cli {

namespace {

std::ofstream output_stream(const std::string & filename) {
    const std::filesystem::path path(filename);
    if (const auto parent = path.parent_path(); !parent.empty()) std::filesystem::create_directories(parent);
    std::ofstream output(path);
    if (!output.is_open()) throw std::runtime_error("Failed to write file: " + filename);
    return output;
}

void finish_output(std::ofstream & output, const std::string & filename) {
    output.flush();
    if (!output) throw std::runtime_error("Failed to complete file write: " + filename);
}

} // namespace

void write_json_file(const std::string & filename, const nlohmann::json & value) {
    auto output = output_stream(filename);
    output << value.dump(2) << '\n';
    finish_output(output, filename);
}

void write_text_file(const std::string & filename, std::string_view value) {
    auto output = output_stream(filename);
    output << value << '\n';
    finish_output(output, filename);
}

} // namespace markov::trace_graph::cli
