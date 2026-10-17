#include "markov/contracts/service_work.hpp"

#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace markov {

DeviceCommandWork DeviceCommandWork::read(const nlohmann::json & value) { return { input::string_value(value.at("task_type")) }; }

nlohmann::json DeviceCommandWork::describe() const {
    return {
        { "task_type", task_type }
    };
}

nlohmann::json PublicationMemberWork::describe() const {
    return {
        {                "request_id",                            request_id },
        {          "generated_tokens",                      generated_tokens },
        {                     "emits",                                 emits },
        {             "output_tokens",                         output_tokens },
        {     "decode_context_tokens",                 decode_context_tokens },
        {    "previous_output_tokens",                previous_output_tokens },
        { "decode_read_offset_tokens",             decode_read_offset_tokens },
        {                  "finishes",                              finishes },
        {              "first_output",  emits && previous_output_tokens == 0 },
        {             "decode_tokens", output_tokens + decode_context_tokens }
    };
}

nlohmann::json PublicationWork::describe() const {
    auto rows = nlohmann::json::array();
    for (const auto & member : members) rows.push_back(member.describe());

    return {
        {    "source_span",                      source_span },
        {        "members",                  std::move(rows) },
        { "sender_enabled",                   sender_enabled },
        {         "branch", "periodic_non_streaming_outputs" }
    };
}

nlohmann::json ServiceWork::describe() const {
    nlohmann::json result{
        {      "kind", SourceGraph::span_name(node_kind) },
        { "node_kind", SourceGraph::span_name(node_kind) }
    };
    if (kernel) {
        result["kind"] = "compute";
        result["kernel"] = kernel->describe();
    }
    if (communication) {
        result["kind"] = "communication";
        result["communication"] = communication->describe();
    }
    if (transfer) {
        result["kind"] = "transfer";
        result["transfer"] = transfer->describe();
    }
    if (device_command) result["device_command"] = device_command->describe();
    return result;
}

} // namespace markov
