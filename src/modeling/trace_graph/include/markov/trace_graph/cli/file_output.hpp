/**
 * @file
 * @brief Checked filesystem writers shared by CLI artifact serializers.
 */
#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace markov::trace_graph::cli {

/** @brief Writes indented JSON, creating parent directories and rejecting I/O failure. */
void write_json_file(const std::string & filename, const nlohmann::json & value);

} // namespace markov::trace_graph::cli
