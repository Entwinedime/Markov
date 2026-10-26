#include "markov/adapters/frameworks/sglang/kv_transfer.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

namespace markov::sglang {

KvTransfers::KvTransfers(std::span<const TemplateAsset * const> prefill, std::span<const TemplateAsset * const> decode, std::int64_t page_size) {
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> pairs;
    for (const auto * destination : decode) {
        const auto & target = destination->environment();
        const auto target_rank = target.at("rank").get<std::size_t>();
        const auto & destination_layout = target.at("kv_layout");
        if (destination_layout.at("page_size") != page_size) throw std::invalid_argument("destination KV page layout differs from its target");

        for (const auto & buffer : destination_layout.at("buffers")) {
            const auto first = buffer.at("head_begin").get<std::int64_t>();
            const auto last = buffer.at("head_end").get<std::int64_t>();
            if (first < 0 || first >= last || buffer.at("head_bytes").get<std::int64_t>() <= 0)
                throw std::invalid_argument("registered KV buffer has an invalid head interval");
            std::vector<std::pair<std::int64_t, std::int64_t>> covered;

            for (const auto * source : prefill) {
                const auto & origin = source->environment();
                const auto source_rank = origin.at("rank").get<std::size_t>();
                const auto & source_layout = origin.at("kv_layout");
                if (source_layout.at("page_size") != page_size) throw std::invalid_argument("source KV page layout differs from its target");
                const auto grouping = source_layout.at("copy_grouping").get<std::string>();
                if (grouping != "buffers" && grouping != "buffer_tokens") throw std::invalid_argument("unknown native KV copy grouping");

                for (const auto & candidate : source_layout.at("buffers")) {
                    if (candidate.at("kind") != buffer.at("kind") || candidate.at("layer") != buffer.at("layer")) continue;
                    const auto source_begin = candidate.at("head_begin").get<std::int64_t>();
                    const auto source_end = candidate.at("head_end").get<std::int64_t>();
                    if (source_begin < 0 || source_begin >= source_end) throw std::invalid_argument("registered source KV head interval is invalid");
                    const auto begin = std::max(first, source_begin);
                    const auto end = std::min(last, source_end);
                    if (begin >= end) continue;
                    if (candidate.at("head_bytes") != buffer.at("head_bytes")) throw std::invalid_argument("KV token/head layout differs between instances");
                    if (grouping == "buffers" && (begin != first || end != last || begin != source_begin || end != source_end))
                        throw std::invalid_argument("native whole-buffer transfer does not describe this head slice");

                    const auto [position, added] = pairs.try_emplace({ source_rank, target_rank }, copies_.size());
                    if (added) {
                        if (source_layout.at("metadata_item_bytes") != destination_layout.at("metadata_item_bytes"))
                            throw std::invalid_argument("registered KV metadata layouts differ between instances");
                        copies_.push_back({ source_rank,
                                            target_rank,
                                            grouping == "buffer_tokens",
                                            {},
                                            source_layout.at("metadata_item_bytes").get<std::vector<std::int64_t>>() });
                    }
                    copies_[position->second].slices.push_back({ candidate.at("kind").get<std::string>(),
                                                                 candidate.at("layer").get<std::int64_t>(),
                                                                 begin,
                                                                 end,
                                                                 begin - source_begin,
                                                                 begin - first,
                                                                 buffer.at("head_bytes").get<std::int64_t>() });
                    covered.emplace_back(begin, end);
                }
            }

            // Each destination head has one writer; duplicated replicas need native writer-selection facts.
            std::ranges::sort(covered);
            auto cursor = first;
            for (const auto & [begin, end] : covered) {
                if (begin != cursor) throw std::invalid_argument("KV transfer has missing or ambiguous source heads");
                cursor = end;
            }
            if (cursor != last) throw std::invalid_argument("KV transfer does not cover the destination buffer");
        }
    }
}

std::span<const KvTransfer> KvTransfers::copies() const { return copies_; }

} // namespace markov::sglang
