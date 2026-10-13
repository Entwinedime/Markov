#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace markov::input {

using Json = nlohmann::json;
using Path = std::filesystem::path;

Json read_json(const Path & path) {
    std::ifstream stream;
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.open(path);

    try {
        return Json::parse(stream);
    }
    catch (const Json::exception & error) {
        throw std::invalid_argument(path.string() + ": " + error.what());
    }
}

void fields(const Json & value, std::initializer_list<std::string_view> names) {
    if (!value.is_object()) { throw std::invalid_argument("expected a JSON object"); }

    for (const auto & item : value.items()) {
        if (std::find(names.begin(), names.end(), item.key()) == names.end()) { throw std::invalid_argument("unknown field: " + item.key()); }
    }
}

std::string string_value(const Json & value) {
    const auto result = value.get<std::string>();
    if (result.empty()) { throw std::invalid_argument("expected a nonempty string"); }

    return result;
}

// JSON numeric conversion otherwise silently truncates floats or narrows integers.
std::int64_t integer(const Json & value, std::int64_t minimum, std::int64_t maximum) {
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(maximum))) {
        throw std::invalid_argument("expected an integer in the declared range");
    }

    const auto result = value.get<std::int64_t>();
    if (result < minimum || result > maximum) { throw std::invalid_argument("integer outside the declared range"); }

    return result;
}

std::uint32_t positive(const Json & value) { return static_cast<std::uint32_t>(integer(value, 1, std::numeric_limits<std::uint32_t>::max())); }

Path resolve_path(const Path & declaring_file, const Json & value) { return (declaring_file.parent_path() / string_value(value)).lexically_normal(); }

} // namespace markov::input
