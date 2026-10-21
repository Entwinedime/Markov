#include "markov/contracts/service_work.hpp"

#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <limits>

namespace markov {

DeviceCommandWork DeviceCommandWork::read(const nlohmann::json & value) { return { input::string_value(value.at("task_type")) }; }

nlohmann::json DeviceCommandWork::describe() const {
    return {
        { "task_type", task_type }
    };
}

HostMessageWork HostMessageWork::read(const nlohmann::json & value) {
    input::fields(value, { "root", "wire_bytes", "incoming_requests" });
    HostMessageWork result{ static_cast<std::uint32_t>(input::integer(value.at("root"), 0, std::numeric_limits<std::uint32_t>::max())), std::nullopt, {} };
    if (!value.at("wire_bytes").is_null()) result.wire_bytes = input::integer(value.at("wire_bytes"), 0, std::numeric_limits<std::int64_t>::max());
    for (const auto & request : value.at("incoming_requests")) {
        result.incoming_requests.emplace_back(input::integer(request.at(0), 0, std::numeric_limits<std::uint32_t>::max()),
                                              input::integer(request.at(1), 0, std::numeric_limits<std::uint32_t>::max()));
    }
    return result;
}

nlohmann::json HostMessageWork::describe() const {
    return {
        {              "root",                                                               root },
        {        "wire_bytes", wire_bytes ? nlohmann::json(*wire_bytes) : nlohmann::json(nullptr) },
        { "incoming_requests",                                                  incoming_requests }
    };
}

CollectiveWork CollectiveWork::read(const nlohmann::json & value) {
    input::fields(value, { "operation", "ranks", "input_shape", "data_type", "host_message" });
    CollectiveWork result{ input::string_value(value.at("operation")), {}, {}, input::string_value(value.at("data_type")) };
    for (const auto & rank : value.at("ranks")) result.ranks.push_back(input::integer(rank, 0, std::numeric_limits<std::uint32_t>::max()));
    for (const auto & dimension : value.at("input_shape")) result.input_shape.push_back(input::integer(dimension, 0, std::numeric_limits<std::int64_t>::max()));
    if (!value.at("host_message").is_null()) result.host_message = HostMessageWork::read(value.at("host_message"));
    return result;
}

nlohmann::json CollectiveWork::describe() const {
    return {
        {    "operation",                                                         operation },
        {        "ranks",                                                             ranks },
        {  "input_shape",                                                       input_shape },
        {    "data_type",                                                         data_type },
        { "host_message", host_message ? host_message->describe() : nlohmann::json(nullptr) }
    };
}

} // namespace markov
