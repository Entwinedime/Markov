#include "markov/contracts/environment.hpp"

#include "markov/contracts/json_input.hpp"

#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace markov {

Environment Environment::read(const nlohmann::json & value) {
    input::fields(value, { "runtime", "service", "devices", "image_id", "measurement" });
    if (value.value("runtime", std::string("docker")) != "docker") throw std::invalid_argument("environment.runtime must be docker");

    Environment result{ input::string_value(value.at("service")), {}, value.value("image_id", std::string{}) };
    if (!result.image_id.empty() && (!result.image_id.starts_with("sha256:") || result.image_id.size() != 71))
        throw std::invalid_argument("environment.image_id must be an immutable Docker image ID");

    const auto & devices = value.at("devices");
    if (!devices.is_array() || devices.empty()) throw std::invalid_argument("environment.devices must be a nonempty array");
    std::set<std::uint32_t> identities;
    for (const auto & device : devices) {
        const auto identity = static_cast<std::uint32_t>(input::integer(device, 0, std::numeric_limits<std::uint32_t>::max()));
        if (!identities.insert(identity).second) throw std::invalid_argument("duplicate device number");
        result.devices.push_back(identity);
    }

    result.measurement = value.value("measurement", nlohmann::json(nullptr));
    if (!result.measurement.is_null()) {
        input::fields(result.measurement, { "cpu", "device", "runtime" });
        for (const std::string_view part : { "cpu", "device", "runtime" }) {
            const auto & field = result.measurement.at(part);
            if (!field.is_null() && !(part == "device" ? field.is_array() && !field.empty() : field.is_object() && !field.empty()))
                throw std::invalid_argument("invalid measurement environment component: " + std::string(part));
        }
    }
    return result;
}

nlohmann::json Environment::describe() const {
    return {
        {     "runtime",    "docker" },
        {     "service",     service },
        {     "devices",     devices },
        {    "image_id",    image_id },
        { "measurement", measurement }
    };
}

bool Environment::shares_measurement(const Environment & other, MeasurementScope scope) const {
    const auto & source = measurement;
    const auto & target = other.measurement;
    if (source.is_null() || target.is_null()) return false;
    if (source.at("runtime").is_null() || source.at("runtime") != target.at("runtime")) return false;

    // Device-only work does not inherit a host condition. Native CPU tables
    // also contain submission/readiness work and retain both resource contexts.
    if (scope == MeasurementScope::HostDevice && (source.at("cpu").is_null() || source.at("cpu") != target.at("cpu"))) return false;
    if (source.at("device").is_null() || source.at("device") != target.at("device")) return false;
    return true;
}

} // namespace markov
