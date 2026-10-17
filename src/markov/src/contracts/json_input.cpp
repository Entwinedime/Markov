#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <stdexcept>

namespace markov::input {

using Json = nlohmann::json;
using Path = std::filesystem::path;

Json read_json(const Path & path) {
    try {
        if (path.extension() == ".gz") {
            std::unique_ptr<gzFile_s, decltype(&gzclose)> stream(gzopen(path.c_str(), "rb"), &gzclose);
            if (!stream) throw std::runtime_error("cannot open compressed JSON: " + path.string());

            std::string content;
            std::array<char, 64 * 1'024> buffer;
            int bytes;
            while ((bytes = gzread(stream.get(), buffer.data(), buffer.size())) > 0) content.append(buffer.data(), bytes);

            // A missing trailer may return all JSON bytes before reporting an
            // incomplete stream. Publication requires the complete gzip member.
            int error;
            const auto message = gzerror(stream.get(), &error);
            if (error != Z_OK) throw std::runtime_error(path.string() + ": " + message);

            return Json::parse(content);
        }

        std::ifstream stream;
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream.open(path);
        return Json::parse(stream);
    }
    catch (const Json::exception & error) {
        throw std::invalid_argument(path.string() + ": " + error.what());
    }
    catch (const std::ios_base::failure & error) {
        throw std::runtime_error(path.string() + ": " + error.what());
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
