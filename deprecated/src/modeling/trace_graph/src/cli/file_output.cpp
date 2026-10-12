/**
 * @file
 * @brief Writes checked JSON artifacts.
 */
#include "markov/trace_graph/cli/file_output.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace markov::trace_graph::cli {

void write_json_file(const std::string & filename, const nlohmann::json & value) {
    const std::filesystem::path path(filename);
    if (const auto parent = path.parent_path(); !parent.empty()) std::filesystem::create_directories(parent);
    std::ofstream output(path);
    if (!output.is_open()) throw std::runtime_error("Failed to write file: " + filename);

    output << value.dump(2) << '\n';
    output.flush();
    if (!output) throw std::runtime_error("Failed to complete file write: " + filename);
}

} // namespace markov::trace_graph::cli
