#pragma once

#include "markov/templates/kv_parameters.hpp"

#include <cstddef>
#include <optional>
#include <span>

namespace markov {

struct SourceRequest;
struct SourceNativeRegion;
struct TemplateDefinition;
struct TemplateNode;

// Compile the observed cold, non-overlap allocator paths to target relations.
class PagedKvLayout {
public:
    // Ordinary ACLNN ConcatD groups at most 32 inputs on the qualified NPU path.
    static constexpr std::size_t release_concat_limit = 32;

    static std::expected<std::optional<KvParameterLayout>, std::string> bind(const TemplateDefinition & source, const TemplateNode & node,
                                                                             const std::optional<std::vector<SourceRequest>> & finished,
                                                                             std::uint32_t page_size_tokens);

private:
    static std::expected<KvParameterLayout, std::string> release(const SourceKernel & source, const SourceNativeRegion & region,
                                                                 std::span<const SourceRequest> finished, std::uint32_t page_size_tokens,
                                                                 std::optional<std::size_t> request_slot = std::nullopt);
    static std::expected<KvParameterLayout, std::string> decode(const SourceKernel & source, const SourceNativeRegion & region, std::uint32_t page_size_tokens);
    static std::expected<KvParameterLayout, std::string> extend(const SourceKernel & source, const SourceNativeRegion & region,
                                                                std::span<const SourceRequest> requests, std::uint32_t page_size_tokens);
};

} // namespace markov
