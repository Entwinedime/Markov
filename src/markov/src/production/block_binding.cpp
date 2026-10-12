#include "markov/production/block_binding.hpp"

#include "markov/contracts/source_observations.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace markov {

SourceNativeRegion NativeRegionBinding::read(const nlohmann::json & metadata) const {
    SourceNativeRegion region{ std::string(semantic),
                               metadata.at("module_path").get<std::string>(),
                               metadata.at("module_type").get<std::string>(),
                               metadata.at("method").get<std::string>(),
                               {},
                               {},
                               {},
                               {},
                               {} };
    if (region.module_type != module_type || region.method != method) {
        throw std::invalid_argument("model region native implementation disagrees with binding: " + std::string(identity));
    }
    if (!request_id_path.empty()) region.request_id = metadata.at("request_id").get<std::string>();

    for (const auto & parameter : parameters) {
        region.parameters.emplace(parameter.name, metadata.at("module_parameters").at(std::string(parameter.name)).get<std::int64_t>());
    }
    for (const auto & parameter : call_parameters) {
        region.call_parameters.emplace(parameter.name, metadata.at("call_parameters").at(std::string(parameter.name)).get<std::int64_t>());
    }

    for (const auto name : inputs) region.inputs.emplace(name, SourceTensorLayout::read(metadata.at("inputs").at(std::string(name))));
    for (const auto & input : module_inputs) region.inputs.emplace(input.name, SourceTensorLayout::read(metadata.at("inputs").at(std::string(input.name))));
    for (const auto & output : outputs) region.outputs.emplace(output.name, SourceTensorLayout::read(metadata.at("outputs").at(std::string(output.name))));

    for (const auto name : cpu_inputs) region.cpu_values.emplace(name, metadata.at("cpu_values").at(std::string(name)).get<std::vector<std::int64_t>>());

    const auto observed = metadata.find("result");
    if (observed != metadata.end()) region.result = observed->get<nlohmann::json::object_t>();

    return region;
}

} // namespace markov
