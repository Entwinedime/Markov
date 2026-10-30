#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace markov {

enum class IndexService { Match, EmptyMatch, Insert, InsertPath, Split, Acquire, Release, RetainRequest, ReleaseRequest, Evict };

struct IndexCall {
    IndexService service;
    bool enter;
};

struct IndexRead {
    std::size_t request;
    std::size_t output;
    std::int64_t begin;
    std::int64_t tokens;

    // The core supplies the immutable map snapshot before a request replaces its cached prefix.
    std::size_t slot = 0;
    std::vector<std::size_t> pages{};
};

struct IndexSlice {
    std::size_t input;
    std::size_t output;
    std::int64_t begin;
    std::int64_t tokens;
};

struct IndexCopy {
    std::size_t input;
    std::size_t output;
};

struct IndexConcat {
    std::vector<std::size_t> inputs;
    std::size_t output;
};

struct IndexWrite {
    std::size_t request;
    std::size_t input;
    std::int64_t begin;
    std::int64_t tokens;
    std::size_t slot = 0;
};

enum class IndexReleaseMode { Immediate, Deferred, Group };

struct IndexRelease {
    IndexReleaseMode mode;
    std::vector<std::size_t> inputs;
    std::vector<std::size_t> pages;
    std::int64_t pending_tokens_before;
};

using IndexOperation = std::variant<IndexCall, IndexRead, IndexSlice, IndexCopy, IndexConcat, IndexWrite, IndexRelease>;

struct PrefixBinding {
    std::size_t request;
    std::size_t node;
    std::int64_t tokens;
};

struct RequestPrefix {
    std::size_t request;
    std::size_t indices;
};

// Index work can finish before a model batch exists; it never carries fabricated batch members.
struct IndexWork {
    static IndexWork read(const nlohmann::json & source);

    std::vector<IndexOperation> operations;
    std::vector<std::size_t> live_indices;
    std::optional<PrefixBinding> binding;
    std::optional<RequestPrefix> prefix;
    std::optional<std::size_t> release_request;
};

} // namespace markov
