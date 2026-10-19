#include "markov/performance/communication_cost_model.hpp"

#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <stdexcept>

namespace markov {

CommunicationCostFeatures CommunicationCostFeatures::read(const nlohmann::json & data) {
    const auto name = data.at("kind").get<std::string>();
    SourceSpanKind kind;
    if (name == "dma") kind = SourceSpanKind::Dma;
    else if (name == "device_marker") kind = SourceSpanKind::DeviceMarker;
    else throw std::invalid_argument("communication cost requires a physical data or command task");

    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto rank_maximum = std::numeric_limits<std::uint32_t>::max();
    return { kind,
             input::string_value(data.at("operation")),
             static_cast<std::uint64_t>(input::integer(data.at("bytes"), 0, maximum)),
             input::string_value(data.at("data_type")),
             input::string_value(data.at("transport")),
             static_cast<std::uint32_t>(input::integer(data.at("source_rank"), 0, rank_maximum)),
             static_cast<std::uint32_t>(input::integer(data.at("destination_rank"), 0, rank_maximum)) };
}

nlohmann::json CommunicationCostFeatures::describe() const {
    return {
        {             "kind", SourceGraph::span_name(kind) },
        {        "operation",                    operation },
        {            "bytes",                        bytes },
        {        "data_type",                    data_type },
        {        "transport",                    transport },
        {      "source_rank",                  source_rank },
        { "destination_rank",             destination_rank }
    };
}

} // namespace markov
