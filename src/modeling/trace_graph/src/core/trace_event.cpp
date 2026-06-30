/**
 * @file
 * @brief TraceEvent 参数读取和最小 JSON 输出工具实现。
 */
#include "markov/trace_graph/core/trace_event.hpp"

#include <cctype>

namespace markov::trace_graph::core {

bool TraceEvent::has_arg(const std::string & key) const { return args.contains(key); }

std::string TraceEvent::arg(const std::string & key, const std::string & fallback) const {
    /**
     * @brief args 是统一字段入口；调用方不需要关心字段来自顶层还是原始 args。
     */
    if (const auto it = args.find(key); it != args.end()) return it->second;
    return fallback;
}

uint64_t TraceEvent::arg_u64(const std::string & key, uint64_t fallback) const {
    /**
     * @brief 支持数字字符串和浮点字符串，最终截断为 uint64。
     *
     * trace timestamp/duration 不应为负数；负数按缺失处理。
     */
    const auto text = arg(key);
    if (text.empty()) return fallback;
    try {
        const double value = std::stod(text);
        if (value < 0.0) return fallback;
        return static_cast<uint64_t>(value);
    }
    catch (...) {
        return fallback;
    }
}

std::string escape_json(const std::string & input) {
    /**
     * @brief Chrome trace 输出只需要最小字符串转义；非 ASCII 字符按原字节写出。
     */
    std::string escaped;
    escaped.reserve(input.size());
    for (char c : input) {
        switch (c) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                constexpr char digits[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped += digits[(static_cast<unsigned char>(c) >> 4) & 0xf];
                escaped += digits[static_cast<unsigned char>(c) & 0xf];
            }
            else { escaped += c; }
        }
    }
    return escaped;
}

} // namespace markov::trace_graph::core
