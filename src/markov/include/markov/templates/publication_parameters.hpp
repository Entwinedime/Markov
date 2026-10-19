#pragma once

#include "markov/contracts/control.hpp"
#include "markov/contracts/service_work.hpp"

#include <cstdint>
#include <expected>
#include <string>

namespace markov {

struct PublicationOutput {
    bool emits;
    std::uint64_t tokens;
    std::uint64_t previous_tokens;

    bool operator==(const PublicationOutput &) const = default;
};

// Cold, non-streaming output: emit periodically and on request completion.
// The producer supplies the effective positive interval; there is no default.
struct PeriodicOutputPolicy {
    std::uint32_t interval_tokens;

    PublicationOutput evaluate(std::uint32_t generated_tokens, bool finishes) const;
};


// A qualified shared output region scans ordered members and packs outputs.
// Its source span is a retained work anchor, not an indivisible CPU cost unit.
// The native interval and context window determine incremental offsets from
// published progress; no second mutable request state is needed.
class PublicationParameterLayout {
    friend class TemplateAssetCodec;

public:
    PublicationParameterLayout(SourceSpanId source_span, std::uint32_t interval_tokens, std::uint32_t context_limit, bool sender_enabled)
        : source_span_(source_span),
          interval_tokens_(interval_tokens),
          context_limit_(context_limit),
          sender_enabled_(sender_enabled) {}

    std::expected<PublicationWork, std::string> evaluate(const BatchInput & input) const;
    bool can_reuse_members_from(const PublicationParameterLayout & donor) const;

private:
    SourceSpanId source_span_;
    std::uint32_t interval_tokens_;
    std::uint32_t context_limit_;
    bool sender_enabled_;
};

} // namespace markov
