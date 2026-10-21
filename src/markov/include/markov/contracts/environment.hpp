#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace markov {

enum class MeasurementScope { Device, HostDevice };

struct Environment {
    std::string service;
    std::vector<std::uint32_t> devices;
    std::string image_id;
    nlohmann::json measurement = nullptr; // Actual CPU/device/runtime conditions, independent of their deployment locations.

    static Environment read(const nlohmann::json & value);
    nlohmann::json describe() const;
    bool shares_implementation(const Environment & other) const { return !image_id.empty() && image_id == other.image_id; }
    bool shares_measurement(const Environment & other, MeasurementScope scope) const;
};

} // namespace markov
