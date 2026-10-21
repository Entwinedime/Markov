#pragma once

#include <cstdint>
#include <string>

namespace markov {

// Source facts shared by extraction and compiled templates. Native binding
// interpretation belongs to production; these values carry no extractor API.
struct SourceRequest {
    std::string id;
    std::uint32_t input_tokens;
    std::uint32_t context_tokens;
    std::uint32_t prompt_tokens;
    std::uint32_t generated_tokens;
};

struct SourceIncomingRequest {
    std::string id;
    std::uint32_t prompt_tokens;
    std::uint32_t output_tokens;
};

} // namespace markov
