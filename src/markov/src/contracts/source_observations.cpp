#include "markov/contracts/source_observations.hpp"

namespace markov {

nlohmann::json SourceTensorLayout::describe() const {
    return {
        {  "shape",  shape },
        { "stride", stride },
        {  "dtype",  dtype },
        { "device", device }
    };
}

std::optional<SourceTensorLayout> SourceTensorLayout::read(const nlohmann::json & value) {
    if (value.is_null()) return std::nullopt;
    return SourceTensorLayout{ value.at("shape").get<std::vector<std::int64_t>>(),
                               value.at("stride").get<std::vector<std::int64_t>>(),
                               value.at("dtype").get<std::string>(),
                               value.at("device").get<std::string>() };
}

nlohmann::json SourceNativeRegion::describe() const {
    auto layouts = nlohmann::json::object();
    for (const auto & [name, layout] : inputs) layouts[name] = layout ? layout->describe() : nlohmann::json(nullptr);

    auto results = nlohmann::json::object();
    for (const auto & [name, layout] : outputs) results[name] = layout ? layout->describe() : nlohmann::json(nullptr);

    nlohmann::json result{
        {          "semantic",        semantic },
        {       "module_path",     module_path },
        {       "module_type",     module_type },
        {            "method",          method },
        { "module_parameters",      parameters },
        {   "call_parameters", call_parameters },
        {            "inputs",         layouts },
        {           "outputs",         results },
        {        "cpu_values",      cpu_values }
    };
    if (request_id) result["request_id"] = *request_id;
    if (!this->result.is_null()) result["result"] = this->result;
    return result;
}

} // namespace markov
