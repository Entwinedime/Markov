#include "trace_graph/core/trace_event.hpp"

#include <cctype>
#include <sstream>

namespace TraceGraph {

bool TraceEvent::has_arg(const std::string & key) const { return args.find(key) != args.end(); }

std::string TraceEvent::arg(const std::string & key, const std::string & fallback) const {
    /**
     * @brief args 是统一字段入口；调用方不需要关心字段来自顶层还是原始 args。
     */
    auto it = args.find(key);
    return it == args.end() ? fallback : it->second;
}

uint64_t TraceEvent::arg_u64(const std::string & key, uint64_t fallback) const {
    /**
     * @brief 支持数字字符串和浮点字符串，最终截断为 uint64。
     *
     * trace timestamp/duration 不应为负数；负数按缺失处理。
     */
    auto it = args.find(key);
    if (it == args.end() || it->second.empty()) return fallback;
    try {
        double value = std::stod(it->second);
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
    std::ostringstream oss;
    for (char c : input) {
        switch (c) {
        case '\\':
            oss << "\\\\";
            break;
        case '"':
            oss << "\\\"";
            break;
        case '\n':
            oss << "\\n";
            break;
        case '\r':
            oss << "\\r";
            break;
        case '\t':
            oss << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                oss << "\\u";
                oss << "00";
                constexpr char digits[] = "0123456789abcdef";
                oss << digits[(static_cast<unsigned char>(c) >> 4) & 0xf] << digits[static_cast<unsigned char>(c) & 0xf];
            }
            else {
                oss << c;
            }
        }
    }
    return oss.str();
}

} // namespace TraceGraph
