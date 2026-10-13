#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>

namespace markov::input {

nlohmann::json read_json(const std::filesystem::path & path);
void fields(const nlohmann::json & value, std::initializer_list<std::string_view> names);
std::string string_value(const nlohmann::json & value);
std::int64_t integer(const nlohmann::json & value, std::int64_t minimum, std::int64_t maximum);
std::uint32_t positive(const nlohmann::json & value);
std::filesystem::path resolve_path(const std::filesystem::path & declaring_file, const nlohmann::json & value);

} // namespace markov::input
