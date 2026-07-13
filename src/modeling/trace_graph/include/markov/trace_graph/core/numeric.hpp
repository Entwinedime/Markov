/**
 * @file
 * @brief Strict numeric conversion helpers shared by CLI, trace, and model parsers.
 */
#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace markov::trace_graph::core {

/** @brief Adds two unsigned values or throws before wraparound can corrupt model state. */
[[nodiscard]] inline uint64_t checked_add_u64(uint64_t left, uint64_t right, std::string_view context) {
    if (right > std::numeric_limits<uint64_t>::max() - left) throw std::overflow_error(std::string(context));
    return left + right;
}

/** @brief Advances a monotonic unsigned counter or throws before it can wrap to zero. */
[[nodiscard]] inline uint64_t checked_increment_u64(uint64_t & value, std::string_view context) {
    value = checked_add_u64(value, 1, context);
    return value;
}

/** @brief Subtracts unsigned values or throws when the decrement violates an invariant. */
[[nodiscard]] inline uint64_t checked_subtract_u64(uint64_t value, uint64_t decrement, std::string_view context) {
    if (decrement > value) throw std::underflow_error(std::string(context));
    return value - decrement;
}

/** @brief Multiplies two unsigned values or throws before wraparound can corrupt model state. */
[[nodiscard]] inline uint64_t checked_multiply_u64(uint64_t left, uint64_t right, std::string_view context) {
    if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) throw std::overflow_error(std::string(context));
    return left * right;
}

/** @brief Computes `ceil(value * multiplier / divisor)` without intermediate overflow. */
[[nodiscard]] inline std::optional<uint64_t> ceil_multiply_divide_u64(uint64_t value, uint64_t multiplier, uint64_t divisor) {
    if (divisor == 0) return std::nullopt;
    using WideUnsigned = unsigned __int128;
    const auto numerator = static_cast<WideUnsigned>(value) * static_cast<WideUnsigned>(multiplier);
    const auto result = (numerator + static_cast<WideUnsigned>(divisor) - 1) / static_cast<WideUnsigned>(divisor);
    if (result > std::numeric_limits<uint64_t>::max()) return std::nullopt;
    return static_cast<uint64_t>(result);
}

/** @brief Computes `floor(value * multiplier / divisor)` without intermediate overflow. */
[[nodiscard]] inline std::optional<uint64_t> floor_multiply_divide_u64(uint64_t value, uint64_t multiplier, uint64_t divisor) {
    if (divisor == 0) return std::nullopt;
    using WideUnsigned = unsigned __int128;
    const auto result = static_cast<WideUnsigned>(value) * static_cast<WideUnsigned>(multiplier) / static_cast<WideUnsigned>(divisor);
    if (result > std::numeric_limits<uint64_t>::max()) return std::nullopt;
    return static_cast<uint64_t>(result);
}

/** @brief Removes ASCII whitespace without allocating a temporary string. */
[[nodiscard]] inline std::string_view trim_ascii(std::string_view value) {
    while (!value.empty() && static_cast<unsigned char>(value.front()) <= ' ') value.remove_prefix(1);
    while (!value.empty() && static_cast<unsigned char>(value.back()) <= ' ') value.remove_suffix(1);
    return value;
}

/**
 * @brief Parses one finite floating-point value and rejects trailing bytes.
 *
 * `std::stod` accepts prefixes such as `12junk` and non-finite values. Configuration
 * and trace contracts require the complete field to be numeric, so this helper uses
 * `from_chars` and treats NaN and infinities as malformed input.
 */
[[nodiscard]] inline std::optional<double> parse_finite_double(std::string_view text) {
    text = trim_ascii(text);
    if (text.empty()) return std::nullopt;
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}

/** @brief Parses a finite value that is greater than or equal to zero. */
[[nodiscard]] inline std::optional<double> parse_nonnegative_double(std::string_view text) {
    const auto value = parse_finite_double(text);
    if (!value || *value < 0.0) return std::nullopt;
    return value;
}

/**
 * @brief Converts a finite non-negative double to uint64_t by truncating fractions.
 *
 * The exclusive upper-bound check avoids undefined behavior at 2^64, which is the
 * rounded double representation of `UINT64_MAX` on IEEE-754 implementations.
 */
[[nodiscard]] inline std::optional<uint64_t> truncate_to_u64(double value) {
    constexpr double kUint64ExclusiveUpperBound = 18'446'744'073'709'551'616.0;
    if (!std::isfinite(value) || value < 0.0 || value >= kUint64ExclusiveUpperBound) return std::nullopt;
    return static_cast<uint64_t>(value);
}

/**
 * @brief Parses an unsigned integer or a non-negative finite decimal representation.
 *
 * Exact integer text is parsed first so `UINT64_MAX` remains representable. Decimal
 * input keeps the prior trace behavior of truncating the fractional component while
 * rejecting malformed, non-finite, negative, and overflowing values.
 */
[[nodiscard]] inline std::optional<uint64_t> parse_u64(std::string_view text) {
    text = trim_ascii(text);
    if (text.empty()) return std::nullopt;

    uint64_t exact = 0;
    const auto [integer_end, integer_error] = std::from_chars(text.data(), text.data() + text.size(), exact);
    if (integer_error == std::errc{} && integer_end == text.data() + text.size()) return exact;

    const auto decimal = parse_finite_double(text);
    return decimal ? truncate_to_u64(*decimal) : std::nullopt;
}

/** @brief Parses only canonical unsigned integer text with no fractional form. */
[[nodiscard]] inline std::optional<uint64_t> parse_exact_u64(std::string_view text) {
    text = trim_ascii(text);
    if (text.empty()) return std::nullopt;
    uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

/** @brief Parses canonical signed integer text with no fractional form. */
[[nodiscard]] inline std::optional<int64_t> parse_i64(std::string_view text) {
    text = trim_ascii(text);
    if (text.empty()) return std::nullopt;
    int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

} // namespace markov::trace_graph::core
