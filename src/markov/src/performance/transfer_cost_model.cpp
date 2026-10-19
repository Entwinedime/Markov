#include "markov/performance/transfer_cost_model.hpp"

#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <stdexcept>

namespace markov {

TransferCostFeatures TransferCostFeatures::read(const nlohmann::json & data) {
    const auto name = data.at("execution").get<std::string>();
    TransferExecution execution;
    if (name == "asynchronous_device") execution = TransferExecution::DeviceAsync;
    else if (name == "synchronous_host_driver") execution = TransferExecution::SynchronousHostDriver;
    else throw std::invalid_argument("unknown transfer execution boundary");

    return { static_cast<std::uint64_t>(input::integer(data.at("bytes"), 0, std::numeric_limits<std::int64_t>::max())),
             SourceTransfer::parse_direction(data.at("direction").get<std::string>()),
             execution };
}

nlohmann::json TransferCostFeatures::describe() const {
    return {
        {     "bytes",                                                                                           bytes },
        { "direction",                                                       SourceTransfer::direction_name(direction) },
        { "execution", execution == TransferExecution::DeviceAsync ? "asynchronous_device" : "synchronous_host_driver" }
    };
}

} // namespace markov
