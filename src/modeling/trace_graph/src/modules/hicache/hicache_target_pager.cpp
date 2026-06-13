/**
 * @file
 * @brief HiCache target page id 投影。
 */
#include "trace_graph/modules/hicache/hicache_target_pager.hpp"

#include <openssl/sha.h>

#include <array>
#include <iomanip>
#include <sstream>

namespace TraceGraph {

namespace {

/** @brief 将十六进制 parent hash 还原为字节，用于 chained page hash。 */
std::vector<unsigned char> hex_to_bytes(const std::string & hex) {
    std::vector<unsigned char> bytes;
    if (hex.size() % 2 != 0) return bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        try {
            bytes.push_back(static_cast<unsigned char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
        }
        catch (...) {
            return {};
        }
    }
    return bytes;
}

/** @brief 将 digest bytes 编码为稳定小写十六进制字符串。 */
std::string bytes_to_hex(const unsigned char * bytes, size_t len) {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) os << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    return os.str();
}

/**
 * @brief 计算单个 target page 的 chained hash。
 *
 * parent hash 参与下一页 hash，保持 page id 对 prefix path 敏感；同一 page token 在
 * 不同前缀下不会意外复用 page id。
 */
std::string hash_token_page(const HiCacheTokenPath & tokens, size_t begin, size_t end, const std::string & prior_hash) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    if (!prior_hash.empty()) {
        auto parent = hex_to_bytes(prior_hash);
        if (!parent.empty()) SHA256_Update(&ctx, parent.data(), parent.size());
    }
    for (size_t index = begin; index < end && index < tokens.size(); ++index) {
        for (const auto word : tokens[index].words) {
            unsigned char raw[4] = {
                static_cast<unsigned char>(word & 0xffu),
                static_cast<unsigned char>((word >> 8u) & 0xffu),
                static_cast<unsigned char>((word >> 16u) & 0xffu),
                static_cast<unsigned char>((word >> 24u) & 0xffu),
            };
            SHA256_Update(&ctx, raw, sizeof(raw));
        }
    }
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256_Final(digest.data(), &ctx);
    return bytes_to_hex(digest.data(), digest.size());
}

} // namespace

HiCacheTargetPager::HiCacheTargetPager(HiCacheConfig config) : config_(std::move(config)) {}

/** @brief target 配置优先；缺省时使用 fact 声明的 source_page_size 作为 invariant。 */
uint64_t HiCacheTargetPager::page_size_for_fact(const HiCacheFact & fact) const {
    if (config_.page_size > 0) return config_.page_size;
    return fact.source_page_size;
}

/** @brief page id 总是包含 cache_scope，避免跨 scope page hash 冲突。 */
std::string HiCacheTargetPager::scoped_page_id(const HiCacheFact & fact, const std::string & page_hash) const {
    const auto scope = fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope;
    return scope + "|" + page_hash;
}

/**
 * @brief 从 token path 生成 target page id 序列。
 *
 * 只对完整 page 生成 id；尾部不足一个 page 的 token 不形成 cache state。
 */
std::vector<std::string> HiCacheTargetPager::pages_for_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) const {
    const auto page_size = page_size_for_fact(fact);
    if (page_size == 0 || tokens.size() < page_size) return {};
    const auto aligned_len = tokens.size() / page_size * page_size;
    std::vector<std::string> pages;
    pages.reserve(aligned_len / page_size);
    std::string parent_hash;
    for (size_t begin = 0; begin < aligned_len; begin += static_cast<size_t>(page_size)) {
        const auto end = begin + static_cast<size_t>(page_size);
        parent_hash = hash_token_page(tokens, begin, end, parent_hash);
        pages.push_back(scoped_page_id(fact, parent_hash));
    }
    return pages;
}

} // namespace TraceGraph
