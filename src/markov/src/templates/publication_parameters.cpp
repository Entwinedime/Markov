#include "markov/templates/publication_parameters.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace markov {

PublicationOutput PeriodicOutputPolicy::evaluate(std::uint32_t generated_tokens, bool finishes) const {
    const auto next = static_cast<std::uint64_t>(generated_tokens) + 1;
    const bool emits = finishes || next % interval_tokens == 0;
    const auto previous = static_cast<std::uint64_t>(generated_tokens / interval_tokens) * interval_tokens;
    return { emits, emits ? next - previous : 0, previous };
}

bool PublicationParameterLayout::can_reuse_members_from(const PublicationParameterLayout & donor) const {
    // Members carry their own target slot; the recipient alone owns shared
    // output membership. Only output policy must agree across member donors.
    return interval_tokens_ == donor.interval_tokens_ && context_limit_ == donor.context_limit_ && sender_enabled_ == donor.sender_enabled_;
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

std::expected<PublicationWork, std::string> PublicationParameterLayout::evaluate(const BatchInput & input) const {
    if (input.phase == BatchPhase::Idle || input.requests.empty()) return std::unexpected("publication_output_membership_not_qualified");

    PublicationWork result{ source_span_, {}, sender_enabled_ };
    for (const auto & request : input.requests) {
        const auto generated = static_cast<std::uint64_t>(request.generated_tokens) + 1;
        // Current output has not yet entered the processed KV sequence.
        const auto sequence = static_cast<std::uint64_t>(request.context_tokens) + request.input_tokens;
        if (sequence <= request.generated_tokens) return std::unexpected("publication_prompt_length_not_qualified");
        const auto context = std::min<std::uint64_t>(sequence - request.generated_tokens, context_limit_);
        const auto output = PeriodicOutputPolicy{ interval_tokens_ }.evaluate(request.generated_tokens, request.finishes);

        // Native output initializes the prompt window once. Later publications
        // extend the decode buffer and transmit only the unsent token suffix.
        result.members.push_back({ request.request_id,
                                   generated,
                                   output.emits,
                                   output.tokens,
                                   output.emits && output.previous_tokens == 0 ? context : 0,
                                   output.previous_tokens,
                                   output.emits ? context : 0,
                                   request.finishes });
    }
    // The qualified CPU region scans target members and always constructs
    // a payload for a nonempty batch, including an empty output list. Member
    // branch replacement is checked by the template planner, separately.
    return result;
}

} // namespace markov
