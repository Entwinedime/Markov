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

/** @brief Exact truncated value plus the first nine decimal digits after the point. */
struct ParsedU64Decimal {
    static constexpr uint32_t kFractionalScale = 1'000'000'000;

    uint64_t integral = 0;
    uint32_t fractional_billionths = 0;
};

/**
 * @brief Parses a non-negative decimal without routing a large integer part through `double`.
 *
 * `integral` is the value truncated toward zero. `fractional_billionths` preserves up to
 * nine digits after the effective decimal point so callers that need sub-unit ordering can
 * avoid independently truncating a start and duration. Scientific notation is supported.
 */
[[nodiscard]] inline std::optional<ParsedU64Decimal> parse_u64_decimal(std::string_view text) {
    text = trim_ascii(text);
    if (text.empty()) return std::nullopt;

    uint64_t exact = 0;
    const auto [integer_end, integer_error] = std::from_chars(text.data(), text.data() + text.size(), exact);
    if (integer_error == std::errc{} && integer_end == text.data() + text.size()) return ParsedU64Decimal{ .integral = exact };

    const auto exponent_offset = text.find_first_of("eE");
    if (exponent_offset != std::string_view::npos && text.find_first_of("eE", exponent_offset + 1) != std::string_view::npos) return std::nullopt;

    const auto mantissa = text.substr(0, exponent_offset);
    if (mantissa.empty() || mantissa.front() == '-' || mantissa.front() == '+') return std::nullopt;
    const auto decimal_offset = mantissa.find('.');
    if (decimal_offset != std::string_view::npos && mantissa.find('.', decimal_offset + 1) != std::string_view::npos) return std::nullopt;

    const auto integer_part = decimal_offset == std::string_view::npos ? mantissa : mantissa.substr(0, decimal_offset);
    const auto fractional_part = decimal_offset == std::string_view::npos ? std::string_view{} : mantissa.substr(decimal_offset + 1);
    if (integer_part.empty() && fractional_part.empty()) return std::nullopt;

    std::string digits;
    digits.reserve(integer_part.size() + fractional_part.size());
    const auto append_digits = [&](std::string_view part) {
        for (const unsigned char value : part) {
            if (value < '0' || value > '9') return false;
            digits.push_back(static_cast<char>(value));
        }
        return true;
    };
    if (!append_digits(integer_part) || !append_digits(fractional_part)) return std::nullopt;

    int64_t exponent = 0;
    if (exponent_offset != std::string_view::npos) {
        auto exponent_text = text.substr(exponent_offset + 1);
        if (exponent_text.empty()) return std::nullopt;
        bool negative = false;
        if (exponent_text.front() == '+' || exponent_text.front() == '-') {
            negative = exponent_text.front() == '-';
            exponent_text.remove_prefix(1);
        }
        if (exponent_text.empty()) return std::nullopt;
        uint64_t magnitude = 0;
        const auto [end, error] = std::from_chars(exponent_text.data(), exponent_text.data() + exponent_text.size(), magnitude);
        if (error != std::errc{} || end != exponent_text.data() + exponent_text.size()
            || magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            return std::nullopt;
        exponent = negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
    }

    using WideSigned = __int128;
    const auto integer_digit_count = static_cast<WideSigned>(integer_part.size()) + static_cast<WideSigned>(exponent);
    const auto source_digit_count = integer_digit_count <= 0                                       ? size_t{ 0 }
                                    : integer_digit_count < static_cast<WideSigned>(digits.size()) ? static_cast<size_t>(integer_digit_count)
                                                                                                   : digits.size();
    uint64_t value = 0;
    for (size_t index = 0; index < source_digit_count; ++index) {
        const auto digit = static_cast<uint64_t>(digits[index] - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    const auto appended_zero_count = integer_digit_count - static_cast<WideSigned>(source_digit_count);
    if (value != 0) {
        if (appended_zero_count > 19) return std::nullopt;
        for (WideSigned index = 0; index < appended_zero_count; ++index) {
            if (value > std::numeric_limits<uint64_t>::max() / 10) return std::nullopt;
            value *= 10;
        }
    }

    uint32_t fractional_billionths = 0;
    for (WideSigned offset = 0; offset < 9; ++offset) {
        fractional_billionths *= 10;
        const auto digit_index = integer_digit_count + offset;
        if (digit_index >= 0 && digit_index < static_cast<WideSigned>(digits.size())) {
            fractional_billionths += static_cast<uint32_t>(digits[static_cast<size_t>(digit_index)] - '0');
        }
    }
    return ParsedU64Decimal{ .integral = value, .fractional_billionths = fractional_billionths };
}

/**
 * @brief Parses an unsigned integer or non-negative decimal and truncates its fraction.
 *
 * Exact text parsing keeps `UINT64_MAX` representable and prevents trace timestamps near
 * 1e15 from rounding into the next integer before truncation.
 */
[[nodiscard]] inline std::optional<uint64_t> parse_u64(std::string_view text) {
    const auto value = parse_u64_decimal(text);
    return value ? std::optional<uint64_t>{ value->integral } : std::nullopt;
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
