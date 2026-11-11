#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/contracts/expression.hpp"

namespace markov {

enum class StateAccess { Read, Write };
StateAccess state_access(std::string_view name);
std::string_view state_access_name(StateAccess access);

struct TokenRegion {
    std::string request;
    std::int64_t begin;
    std::int64_t end;

    static TokenRegion read(const nlohmann::json & source);
    nlohmann::json document() const;
    bool operator==(const TokenRegion &) const = default;
};

// Each member supplies logical token positions; physical pages remain owned by the resource service.
struct TokenRangeRule {
    Expression begin;
    Expression end;

    static TokenRangeRule read(const nlohmann::json & source);
    nlohmann::json document() const;
    std::vector<TokenRegion> evaluate(const nlohmann::json & work) const;
};

// A token's K/V payload is one logical row; transfers may populate only part of it.
struct KvSlice {
    std::int64_t token_bytes;
    std::int64_t begin;
    std::int64_t end;

    static KvSlice read(const nlohmann::json & source);
    nlohmann::json document() const;
    void validate() const;
    std::vector<std::pair<std::int64_t, std::int64_t>> ranges(std::int64_t token_begin, std::int64_t token_end) const;
};

struct KvAccess {
    std::string pool;
    std::int64_t layer;
    StateAccess access;
    KvSlice slice;
    std::vector<TokenRegion> regions;
    std::optional<TokenRangeRule> rule;

    static KvAccess read(const nlohmann::json & source);
    nlohmann::json document() const;
};

// Logical request fields resolve through their declared resource pool, with independent physical lifetimes.
struct SlotAccess {
    std::string pool;
    std::string field;
    StateAccess access;
    std::string members;
    std::vector<std::string> requests;

    // Replicas of one request-scoped publication may write concurrently; different publications remain ordered.
    std::string publication;

    static SlotAccess read(const nlohmann::json & source);
    nlohmann::json document() const;
};

} // namespace markov
