/**
 * @file
 * @brief HiCache target page projection 实现。
 */
#include "markov/trace_graph/modules/hicache/runtime/target_pager.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <iterator>
#include <ranges>
#include <span>
#include <sstream>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {

namespace target_pager_detail {

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
    /**
     * @brief HiCache page hash 是链式投影。
     *
     * 后一页把前一页 hash 纳入输入，使同一 token 序列在不同 page_size 下自然产生不同
     * target page identity。
     */
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

} // namespace target_pager_detail

using target_pager_detail::hash_page;

std::vector<std::string> HiCachePagePath::page_ids() const {
    std::vector<std::string> ids;
    ids.reserve(pages.size());
    std::ranges::transform(pages, std::back_inserter(ids), &HiCacheProjectedPage::id);
    return ids;
}

HiCacheTargetPager::HiCacheTargetPager(frontend::HiCacheConfig config) : config_(std::move(config)) {}

uint64_t HiCacheTargetPager::page_size_for_fact(const HiCacheFact & fact) const {
    /**
     * @brief target config 的 page_size 优先。
     *
     * 缺失时只用 source_page_size 做合同内兜底，不能使用 source 已生成的 page id。
     */
    if (config_.page_size > 0) return config_.page_size;
    return fact.source_page_size;
}

std::string HiCacheTargetPager::scoped_page_id(const std::string & cache_scope, const std::string & page_hash) const {
    /**
     * @brief cache_scope 是 page identity 的一部分。
     *
     * 不同 rank/scope 的同 hash page 不能在模型中合并，否则跨 rank capacity 和
     * storage readable 会串扰。
     */
    return (cache_scope.empty() ? std::string("-1") : cache_scope) + "|" + page_hash;
}

HiCachePagePath HiCacheTargetPager::project(const HiCacheFact & fact, const HiCacheTokenPath & tokens) const {
    /**
     * @brief projection 只产生完整 page。
     *
     * 尾部不足 page_size 的 tokens 属于 request lifecycle 信息，但不能形成 HiCache page residency。
     */
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

} // namespace markov::trace_graph::modules::hicache::runtime
