#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace markov {

// Source facts shared by extraction and compiled templates. Native binding
// interpretation belongs to production; these values carry no extractor API.
struct SourceRequest {
    std::string id;
    std::uint32_t input_tokens;
    std::uint32_t context_tokens;
    std::uint32_t prompt_tokens;
    std::uint32_t generated_tokens;
};

struct SourceTensorLayout {
    std::vector<std::int64_t> shape;
    std::vector<std::int64_t> stride;
    std::string dtype;
    std::string device;

    nlohmann::json describe() const;
    static std::optional<SourceTensorLayout> read(const nlohmann::json & value);
};

struct SourceNativeRegion {
    std::string semantic;
    std::string module_path;
    std::string module_type;
    std::string method;
    std::map<std::string, std::int64_t> parameters;
    std::map<std::string, std::int64_t> call_parameters;
    std::map<std::string, std::optional<SourceTensorLayout>> inputs;
    std::map<std::string, std::optional<SourceTensorLayout>> outputs;
    std::map<std::string, std::vector<std::int64_t>> cpu_values;
    std::optional<std::string> request_id = std::nullopt;
    nlohmann::json result = nullptr;

    nlohmann::json describe() const;
};

} // namespace markov
