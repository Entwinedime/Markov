/**
 * @file
 * @brief Checked filesystem writers shared by CLI artifact serializers.
 */
#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <string_view>

namespace markov::trace_graph::cli {

/** @brief Writes indented JSON, creating parent directories and rejecting I/O failure. */
void write_json_file(const std::string & filename, const nlohmann::json & value);

/** @brief Writes text plus one trailing newline and rejects incomplete output. */
void write_text_file(const std::string & filename, std::string_view value);

} // namespace markov::trace_graph::cli
