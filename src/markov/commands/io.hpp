#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <vector>

namespace markov::commands {

void write_report(const nlohmann::json & result, const std::filesystem::path & destination);
void prepare_output(const std::optional<std::filesystem::path> & output, const std::vector<std::filesystem::path> & inputs);

} // namespace markov::commands
