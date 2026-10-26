#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "markov/templates/asset.hpp"

namespace markov::sglang {

struct KvSlice {
    std::string kind;
    std::int64_t layer;
    std::int64_t head_begin;
    std::int64_t head_end;
    std::int64_t source_head_offset;
    std::int64_t destination_head_offset;
    std::int64_t head_bytes;
};

struct KvTransfer {
    std::size_t source;
    std::size_t destination;
    bool per_buffer_tokens;
    std::vector<KvSlice> slices;
    std::vector<std::int64_t> metadata_item_bytes;
};

// Registered layer/head intervals determine payloads; the observed API determines copy grouping.
class KvTransfers {
public:
    KvTransfers(std::span<const TemplateAsset * const> prefill, std::span<const TemplateAsset * const> decode, std::int64_t page_size);
    std::span<const KvTransfer> copies() const;

private:
    std::vector<KvTransfer> copies_;
};

} // namespace markov::sglang
