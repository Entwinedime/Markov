#include "markov/templates/publication_parameters.hpp"

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

std::expected<SourceSpanId, std::string> PublicationParameterLayout::qualify(const BatchInput & input) const {
    if (input.phase == BatchPhase::Idle || input.requests.empty()) return std::unexpected("publication_output_membership_not_qualified");

    for (const auto & request : input.requests) {
        // Current output has not yet entered the processed KV sequence.
        const auto sequence = static_cast<std::uint64_t>(request.context_tokens) + request.input_tokens;
        if (sequence <= request.generated_tokens) return std::unexpected("publication_prompt_length_not_qualified");
    }
    return source_span_;
}

std::expected<CpuPublicationWork, std::string> PublicationParameterLayout::cpu_work(const BatchInput & input) const {
    if (const auto qualified = qualify(input); !qualified) return std::unexpected(qualified.error());

    CpuPublicationWork result;
    for (const auto & request : input.requests) {
        const auto generated = static_cast<std::uint64_t>(request.generated_tokens) + 1;
        const auto output = PeriodicOutputPolicy{ interval_tokens_ }.evaluate(request.generated_tokens, request.finishes);
        const auto prompt = static_cast<std::uint64_t>(request.context_tokens) + request.input_tokens - request.generated_tokens;
        result.requests.push_back({ output.tokens,
                                    output.emits && output.previous_tokens == 0 ? std::min<std::uint64_t>(prompt, context_limit_) : 0,
                                    output.emits && request.finishes ? generated : 0 });
    }
    return result;
}

} // namespace markov
