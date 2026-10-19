#pragma once

#include "markov/contracts/control.hpp"
#include "markov/graph/source_graph.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace markov {

enum class KvParameterKind { Extend, Decode, Release };
enum class KvParameterExtent { Requests, Tokens, FreePages, ReleasedTokens, ReleasedPages, ReleaseFreePages, ReleaseTotalPages };
enum class KvParameterOutput { None, NewPages, Allocation, Release };

struct KvParameterAxis {
    // The producer qualifies the one-dimensional operand and its slot.
    bool output;
    std::size_t slot;
    KvParameterExtent extent;
};

struct KvParameterDomain {
    // Allocation and free-list branches constrain node parameters. Release
    // membership is qualified once by the containing region.
    std::optional<std::int64_t> maximum_new_pages;
    std::optional<bool> allocates_pages;
    std::optional<bool> release_free_empty;
    std::optional<std::size_t> release_request_slot;
};

class KvParameterLayout {
    friend class TemplateAssetCodec;

public:
    KvParameterLayout(KvParameterKind kind, std::uint32_t page_size, KvParameterDomain domain, std::vector<KvParameterAxis> axes, KvParameterOutput output,
                      bool release_member_inputs = false)
        : kind_(kind),
          page_size_(page_size),
          domain_(std::move(domain)),
          axes_(std::move(axes)),
          output_(output),
          release_member_inputs_(release_member_inputs) {}
    std::expected<SourceKernel, std::string> evaluate(const SourceKernel & source, const BatchInput & input,
                                                      std::optional<std::size_t> request_slot = std::nullopt) const;
    std::optional<bool> allocation_branch() const { return domain_.allocates_pages; }

private:
    KvParameterKind kind_;
    std::uint32_t page_size_;
    KvParameterDomain domain_;
    std::vector<KvParameterAxis> axes_;
    KvParameterOutput output_;
    bool release_member_inputs_;
};

} // namespace markov
