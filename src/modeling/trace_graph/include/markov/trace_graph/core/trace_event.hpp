/**
 * @file
 * @brief In-memory normalized Chrome event with lazy argument access.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::core {

/** @brief Non-owning byte range into a shared trace buffer. */
struct TraceByteRange {
    size_t offset = 0;
    size_t length = 0;
};

/** @brief Transparent hash enabling allocation-free lookup by `std::string_view`. */
struct TraceArgHash {
    using is_transparent = void;

    [[nodiscard]] size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
};

using TraceArgMap = std::unordered_map<std::string, std::string, TraceArgHash, std::equal_to<>>;

/**
 * @brief Unified event representation used throughout the C++ backend.
 *
 * Input can originate from torch profiler, LD_PRELOAD, or Python probes. The reader parses
 * only top-level fields eagerly; `args` remains a slice into the shared trace buffer and
 * is materialized only when a caller requests the complete map.
 */
struct TraceEvent {
    TraceEvent() = default;
    TraceEvent(const TraceEvent & other);
    TraceEvent & operator=(const TraceEvent & other);
    TraceEvent(TraceEvent &&) noexcept = default;
    TraceEvent & operator=(TraceEvent &&) noexcept = default;

    /** @brief Stable input order, retained as the final same-timestamp tie breaker. */
    size_t index = 0;

    /** @brief Optional external event ID; most duration events do not provide one. */
    std::string event_id;

    /** @brief Eagerly parsed Chrome trace fields. */
    std::string name;
    std::string cat;
    char ph = 'X';
    uint64_t ts = 0;
    uint64_t dur = 0;
    std::string pid = "-1";
    std::string tid = "-1";

    /** @brief Points args at a reader-owned buffer slice instead of copying the source text. */
    void set_args_json_slice(std::shared_ptr<const std::string> buffer, const TraceByteRange & range);

    /** @brief Returns raw args for marker checks and single-key lazy parsing. */
    [[nodiscard]] std::string_view args_json_view() const;

    /** @brief Tests the unified raw/override argument view without allocating a key string. */
    [[nodiscard]] bool has_arg(std::string_view key) const;

    /** @brief Tests raw slices and override maps for a key hint without parsing JSON. */
    [[nodiscard]] bool has_arg_key_hint(std::string_view key) const;

    /** @brief Tests the in-memory override table without scanning raw args JSON. */
    [[nodiscard]] bool has_arg_override(std::string_view key) const;

    /** @brief Returns a string argument or a caller-supplied fallback. */
    [[nodiscard]] std::string arg(std::string_view key, std::string_view fallback = {}) const;

    /** @brief Returns one argument while distinguishing a missing key from an empty value. */
    [[nodiscard]] std::optional<std::string> find_arg(std::string_view key) const;

    /** @brief Parses an unsigned argument; invalid or negative input uses the fallback. */
    [[nodiscard]] uint64_t arg_u64(std::string_view key, uint64_t fallback = 0) const;

    /** @brief Writes a synthetic override without materializing raw args. */
    void set_arg(std::string_view key, std::string_view value);

    /** @brief Merges another event's materialized args for torch/LD in-memory channel joins. */
    void merge_args_from(const TraceEvent & other);

    /** @brief Returns the fully materialized map; callers should avoid this on hot paths. */
    [[nodiscard]] const TraceArgMap & args_map() const;

private:
    /** @brief One lazily merged argument source, retained in merge order. */
    struct TraceArgLayer {
        std::shared_ptr<const std::string> buffer{};
        size_t offset = 0;
        size_t length = 0;
        std::shared_ptr<const TraceArgMap> overrides;
    };

    void ensure_args_materialized() const;
    void freeze_arg_overrides();
    void append_arg_layers_from(const TraceEvent & other);
    [[nodiscard]] bool lookup_arg_layers(std::string_view key, std::string * value) const;
    [[nodiscard]] bool lookup_raw_arg(std::string_view key, std::string * value) const;

    mutable bool args_materialized_ = false;
    std::unique_ptr<std::string> owned_args_json_;
    std::shared_ptr<const std::string> args_buffer_;
    size_t args_offset_ = 0;
    size_t args_length_ = 0;
    mutable std::unique_ptr<TraceArgMap> args_;
    mutable std::unique_ptr<TraceArgMap> arg_overrides_;
    std::unique_ptr<std::vector<TraceArgLayer>> arg_layers_;
};

/**
 * @brief Escapes one string for streaming Chrome trace output.
 *
 * Streaming output deliberately avoids a JSON DOM so large DAG traces are not duplicated.
 */
std::string escape_json(std::string_view input);

} // namespace markov::trace_graph::core
