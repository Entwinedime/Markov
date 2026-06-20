/**
 * @file
 * @brief HiCache target page projection。
 */
#include "trace_graph/modules/hicache/hicache_target_pager.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <iterator>
#include <ranges>
#include <span>
#include <sstream>
#include <utility>

namespace TraceGraph {

namespace {

std::vector<unsigned char> hex_to_bytes(const std::string & hex) {
    if (hex.size() % 2 != 0) return {};
    std::vector<unsigned char> bytes;
    bytes.reserve(hex.size() / 2);
    for (const auto index : std::views::iota(size_t{ 0 }, hex.size() / 2)) {
        try {
            bytes.push_back(static_cast<unsigned char>(std::stoul(hex.substr(index * 2, 2), nullptr, 16)));
        }
        catch (...) {
            return {};
        }
    }
    return bytes;
}

std::string to_hex(std::span<const unsigned char> bytes) {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    std::ranges::for_each(bytes, [&](unsigned char byte) { os << std::setw(2) << static_cast<unsigned int>(byte); });
    return os.str();
}

std::string hash_page(const HiCacheTokenPath & tokens, size_t begin, size_t end, const std::string & prior_hash) {
    auto * ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) return "";
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    if (!prior_hash.empty()) {
        const auto parent = hex_to_bytes(prior_hash);
        if (!parent.empty()) EVP_DigestUpdate(ctx, parent.data(), parent.size());
    }
    for (const auto index : std::views::iota(begin, std::min(end, tokens.size()))) {
        std::ranges::for_each(tokens[index].words, [&](uint32_t word) {
            const std::array<unsigned char, 4> raw{
                static_cast<unsigned char>(word & 0xffu),
                static_cast<unsigned char>((word >> 8u) & 0xffu),
                static_cast<unsigned char>((word >> 16u) & 0xffu),
                static_cast<unsigned char>((word >> 24u) & 0xffu),
            };
            EVP_DigestUpdate(ctx, raw.data(), raw.size());
        });
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    EVP_DigestFinal_ex(ctx, digest.data(), &digest_size);
    EVP_MD_CTX_free(ctx);
    return to_hex(std::span<const unsigned char>(digest.data(), digest_size));
}

} // namespace

std::vector<std::string> HiCachePagePath::page_ids() const {
    std::vector<std::string> ids;
    ids.reserve(pages.size());
    std::ranges::transform(pages, std::back_inserter(ids), &HiCacheProjectedPage::id);
    return ids;
}

HiCacheTargetPager::HiCacheTargetPager(HiCacheConfig config) : config_(std::move(config)) {}

uint64_t HiCacheTargetPager::page_size_for_fact(const HiCacheFact & fact) const {
    if (config_.page_size > 0) return config_.page_size;
    return fact.source_page_size;
}

std::string HiCacheTargetPager::scoped_page_id(const std::string & cache_scope, const std::string & page_hash) const {
    return (cache_scope.empty() ? std::string("-1") : cache_scope) + "|" + page_hash;
}

HiCachePagePath HiCacheTargetPager::project(const HiCacheFact & fact, const HiCacheTokenPath & tokens) const {
    HiCachePagePath path;
    path.cache_scope = fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope;
    path.page_size = page_size_for_fact(fact);
    if (path.page_size == 0 || tokens.size() < path.page_size) return path;

    const auto aligned_tokens = tokens.size() / static_cast<size_t>(path.page_size) * static_cast<size_t>(path.page_size);
    const auto page_count = aligned_tokens / static_cast<size_t>(path.page_size);
    path.pages.reserve(page_count);

    std::string prior_hash;
    for (const auto page_index : std::views::iota(size_t{ 0 }, page_count)) {
        const auto begin = page_index * static_cast<size_t>(path.page_size);
        const auto end = begin + static_cast<size_t>(path.page_size);
        prior_hash = hash_page(tokens, begin, end, prior_hash);
        path.pages.push_back(HiCacheProjectedPage{
            .id = scoped_page_id(path.cache_scope, prior_hash),
            .cache_scope = path.cache_scope,
            .hash = prior_hash,
            .page_index = static_cast<uint64_t>(page_index),
            .token_begin = static_cast<uint64_t>(begin),
            .token_end = static_cast<uint64_t>(end),
        });
    }
    return path;
}

std::vector<std::string> HiCacheTargetPager::pages_for_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) const {
    return project(fact, tokens).page_ids();
}

} // namespace TraceGraph
