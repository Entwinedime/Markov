/**
 * @file
 * @brief HiCache target-page projection implementation.
 */
#include "markov/trace_graph/modules/hicache/runtime/target_pager.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {

namespace target_pager_detail {

std::string to_hex(std::span<const unsigned char> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output(bytes.size() * 2, '0');
    for (size_t index = 0; index < bytes.size(); ++index) {
        output[index * 2] = digits[(bytes[index] >> 4u) & 0xfu];
        output[index * 2 + 1] = digits[bytes[index] & 0xfu];
    }
    return output;
}

std::array<unsigned char, 32> hash_page(EVP_MD_CTX * context, const HiCacheTokenPath & tokens, size_t begin, size_t end,
                                        std::span<const unsigned char> prior_digest) {
    // Each page includes the previous digest, preserving prefix identity and naturally
    // producing different page IDs when the target page size changes.
    if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) throw std::runtime_error("Failed to initialize SHA-256 page projection");
    if (!prior_digest.empty() && EVP_DigestUpdate(context, prior_digest.data(), prior_digest.size()) != 1)
        throw std::runtime_error("Failed to update chained SHA-256 page projection");
    for (const auto index : std::views::iota(begin, std::min(end, tokens.size()))) {
        std::ranges::for_each(tokens[index].words, [&](uint32_t word) {
            const std::array<unsigned char, 4> raw{
                static_cast<unsigned char>(word & 0xffu),
                static_cast<unsigned char>((word >> 8u) & 0xffu),
                static_cast<unsigned char>((word >> 16u) & 0xffu),
                static_cast<unsigned char>((word >> 24u) & 0xffu),
            };
            if (EVP_DigestUpdate(context, raw.data(), raw.size()) != 1) throw std::runtime_error("Failed to hash HiCache token word");
        });
    }
    std::array<unsigned char, 32> digest{};
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context, digest.data(), &digest_size) != 1 || digest_size != digest.size())
        throw std::runtime_error("Failed to finalize SHA-256 page projection");
    return digest;
}

} // namespace target_pager_detail

using target_pager_detail::hash_page;
using target_pager_detail::to_hex;

std::vector<std::string> HiCachePagePath::page_ids() const {
    std::vector<std::string> ids;
    ids.reserve(pages.size());
    std::ranges::transform(pages, std::back_inserter(ids), &HiCacheProjectedPage::id);
    return ids;
}

HiCacheTargetPager::HiCacheTargetPager(frontend::HiCacheConfig config) : config_(std::move(config)) {}

uint64_t HiCacheTargetPager::page_size_for_fact(const HiCacheFact & fact) const {
    // Source page size is a contract-approved fallback value, not a source page identity.
    if (config_.page_size > 0) return config_.page_size;
    return fact.source_page_size;
}

std::string HiCacheTargetPager::scoped_page_id(const std::string & cache_scope, const std::string & page_hash) const {
    // Scope prevents equal hashes on different ranks from sharing capacity or readability.
    return (cache_scope.empty() ? std::string("-1") : cache_scope) + "|" + page_hash;
}

HiCachePagePath HiCacheTargetPager::project(const HiCacheFact & fact, const HiCacheTokenPath & tokens) const {
    // Trailing tokens remain request-lifecycle information but cannot form cache residency.
    HiCachePagePath path;
    path.cache_scope = fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope;
    path.page_size = page_size_for_fact(fact);
    if (path.page_size == 0) return path;
    if (path.page_size > std::numeric_limits<size_t>::max()) throw std::overflow_error("HiCache page size exceeds the platform size_t range");
    if (tokens.size() < static_cast<size_t>(path.page_size)) return path;

    const auto aligned_tokens = tokens.size() / static_cast<size_t>(path.page_size) * static_cast<size_t>(path.page_size);
    const auto page_count = aligned_tokens / static_cast<size_t>(path.page_size);
    path.pages.reserve(page_count);

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context) throw std::runtime_error("Failed to allocate SHA-256 page projection context");
    std::array<unsigned char, 32> prior_digest{};
    bool has_prior_digest = false;
    for (const auto page_index : std::views::iota(size_t{ 0 }, page_count)) {
        const auto begin = page_index * static_cast<size_t>(path.page_size);
        const auto end = begin + static_cast<size_t>(path.page_size);
        prior_digest =
            hash_page(context.get(), tokens, begin, end, has_prior_digest ? std::span<const unsigned char>(prior_digest) : std::span<const unsigned char>{});
        has_prior_digest = true;
        const auto page_hash = to_hex(prior_digest);
        path.pages.push_back(HiCacheProjectedPage{
            .id = scoped_page_id(path.cache_scope, page_hash),
            .cache_scope = path.cache_scope,
            .hash = page_hash,
            .page_index = static_cast<uint64_t>(page_index),
            .token_begin = static_cast<uint64_t>(begin),
            .token_end = static_cast<uint64_t>(end),
        });
    }
    return path;
}

} // namespace markov::trace_graph::modules::hicache::runtime
