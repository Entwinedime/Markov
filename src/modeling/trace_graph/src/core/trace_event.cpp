/**
 * @file
 * @brief TraceEvent 参数读取和最小 JSON 输出工具实现。
 */
#include "markov/trace_graph/core/trace_event.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <charconv>
#include <sstream>
#include <string_view>

namespace markov::trace_graph::core {

namespace trace_event_detail {

using Json = nlohmann::json;

std::string scalar_to_string(const Json & value) {
    if (value.is_null()) return "";
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_number_unsigned()) return std::to_string(value.get<uint64_t>());
    if (value.is_number_integer()) return std::to_string(value.get<int64_t>());
    if (value.is_number_float()) {
        std::ostringstream os;
        os << value.get<double>();
        return os.str();
    }
    return value.dump();
}

void flatten_args(const Json & value, const std::string & prefix, std::unordered_map<std::string, std::string> & args) {
    if (!value.is_object()) return;
    for (const auto & item : value.items()) {
        const auto key = prefix.empty() ? item.key() : prefix + "." + item.key();
        args[key] = scalar_to_string(item.value());
        if (item.value().is_object()) flatten_args(item.value(), key, args);
    }
}

std::string_view trim_view(std::string_view value) {
    while (!value.empty() && static_cast<unsigned char>(value.front()) <= ' ') value.remove_prefix(1);
    while (!value.empty() && static_cast<unsigned char>(value.back()) <= ' ') value.remove_suffix(1);
    return value;
}

std::string decode_json_string(std::string_view literal) {
    literal = trim_view(literal);
    if (literal.size() < 2 || literal.front() != '"' || literal.back() != '"') return std::string(literal);
    std::string out;
    out.reserve(literal.size() - 2);
    for (size_t i = 1; i + 1 < literal.size(); ++i) {
        char c = literal[i];
        if (c != '\\' || i + 1 >= literal.size()) {
            out.push_back(c);
            continue;
        }
        char esc = literal[++i];
        switch (esc) {
        case '"':
        case '\\':
        case '/':
            out.push_back(esc);
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'u': {
            if (i + 4 >= literal.size()) break;
            unsigned code = 0;
            auto begin = literal.data() + i + 1;
            auto end = begin + 4;
            auto [ptr, ec] = std::from_chars(begin, end, code, 16);
            if (ec == std::errc{} && ptr == end) {
                if (code <= 0x7f) out.push_back(static_cast<char>(code));
                else out.append(literal.substr(i - 1, 6));
                i += 4;
            }
            break;
        }
        default:
            out.push_back(esc);
            break;
        }
    }
    return out;
}

size_t skip_json_string(std::string_view text, size_t pos) {
    if (pos >= text.size() || text[pos] != '"') return pos;
    ++pos;
    bool escape = false;
    for (; pos < text.size(); ++pos) {
        char c = text[pos];
        if (escape) {
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
        if (c == '"') return pos + 1;
    }
    return text.size();
}

size_t skip_json_value(std::string_view text, size_t pos) {
    while (pos < text.size() && static_cast<unsigned char>(text[pos]) <= ' ') ++pos;
    if (pos >= text.size()) return pos;
    if (text[pos] == '"') return skip_json_string(text, pos);
    if (text[pos] == '{' || text[pos] == '[') {
        const char open = text[pos];
        const char close = open == '{' ? '}' : ']';
        int depth = 0;
        for (; pos < text.size(); ++pos) {
            char c = text[pos];
            if (c == '"') {
                pos = skip_json_string(text, pos);
                if (pos == 0) return text.size();
                --pos;
                continue;
            }
            if (c == open) ++depth;
            else if (c == close) {
                --depth;
                if (depth == 0) return pos + 1;
            }
            else if (open == '{' && c == '[') {
                pos = skip_json_value(text, pos);
                if (pos == 0) return text.size();
                --pos;
            }
            else if (open == '[' && c == '{') {
                pos = skip_json_value(text, pos);
                if (pos == 0) return text.size();
                --pos;
            }
        }
        return text.size();
    }
    while (pos < text.size() && text[pos] != ',' && text[pos] != '}' && text[pos] != ']' && static_cast<unsigned char>(text[pos]) > ' ') ++pos;
    return pos;
}

bool find_top_level_key_value(std::string_view object, std::string_view key, std::string_view & value) {
    object = trim_view(object);
    if (object.empty() || object.front() != '{') return false;
    size_t pos = 1;
    while (pos < object.size()) {
        while (pos < object.size() && static_cast<unsigned char>(object[pos]) <= ' ') ++pos;
        if (pos >= object.size() || object[pos] == '}') return false;
        if (object[pos] != '"') {
            pos = skip_json_value(object, pos);
            if (pos < object.size() && object[pos] == ',') ++pos;
            continue;
        }
        const auto key_begin = pos;
        const auto key_end = skip_json_string(object, pos);
        const auto decoded_key = decode_json_string(object.substr(key_begin, key_end - key_begin));
        pos = key_end;
        while (pos < object.size() && static_cast<unsigned char>(object[pos]) <= ' ') ++pos;
        if (pos >= object.size() || object[pos] != ':') return false;
        ++pos;
        while (pos < object.size() && static_cast<unsigned char>(object[pos]) <= ' ') ++pos;
        const auto value_begin = pos;
        const auto value_end = skip_json_value(object, pos);
        if (decoded_key == key) {
            value = object.substr(value_begin, value_end - value_begin);
            return true;
        }
        pos = value_end;
        while (pos < object.size() && static_cast<unsigned char>(object[pos]) <= ' ') ++pos;
        if (pos < object.size() && object[pos] == ',') ++pos;
    }
    return false;
}

std::string json_value_to_string(std::string_view value) {
    value = trim_view(value);
    if (value.empty() || value == "null") return "";
    if (value.front() == '"') return decode_json_string(value);
    return std::string(value);
}

} // namespace trace_event_detail

TraceEvent::TraceEvent(const TraceEvent & other)
    : index(other.index),
      event_id(other.event_id),
      name(other.name),
      cat(other.cat),
      ph(other.ph),
      ts(other.ts),
      dur(other.dur),
      pid(other.pid),
      tid(other.tid),
      args_json(other.args_json),
      args_materialized_(other.args_materialized_),
      args_buffer_(other.args_buffer_),
      args_offset_(other.args_offset_),
      args_length_(other.args_length_) {
    if (other.args_) args_ = std::make_unique<std::unordered_map<std::string, std::string>>(*other.args_);
    if (other.arg_overrides_) arg_overrides_ = std::make_unique<std::unordered_map<std::string, std::string>>(*other.arg_overrides_);
}

TraceEvent & TraceEvent::operator=(const TraceEvent & other) {
    if (this == &other) return *this;
    index = other.index;
    event_id = other.event_id;
    name = other.name;
    cat = other.cat;
    ph = other.ph;
    ts = other.ts;
    dur = other.dur;
    pid = other.pid;
    tid = other.tid;
    args_json = other.args_json;
    args_materialized_ = other.args_materialized_;
    args_buffer_ = other.args_buffer_;
    args_offset_ = other.args_offset_;
    args_length_ = other.args_length_;
    args_.reset();
    arg_overrides_.reset();
    if (other.args_) args_ = std::make_unique<std::unordered_map<std::string, std::string>>(*other.args_);
    if (other.arg_overrides_) arg_overrides_ = std::make_unique<std::unordered_map<std::string, std::string>>(*other.arg_overrides_);
    return *this;
}

void TraceEvent::set_args_json_slice(std::shared_ptr<const std::string> buffer, size_t offset, size_t length) {
    args_buffer_ = std::move(buffer);
    args_offset_ = offset;
    args_length_ = length;
    args_json.clear();
    args_materialized_ = false;
    args_.reset();
}

std::string_view TraceEvent::args_json_view() const {
    if (args_buffer_ && args_offset_ <= args_buffer_->size() && args_offset_ + args_length_ <= args_buffer_->size()) {
        return std::string_view(args_buffer_->data() + args_offset_, args_length_);
    }
    return args_json;
}

bool TraceEvent::has_arg(const std::string & key) const {
    if (arg_overrides_ && arg_overrides_->contains(key)) return true;
    if (args_materialized_) return args_ && args_->contains(key);
    std::string value;
    return lookup_raw_arg(key, value);
}

std::string TraceEvent::arg(const std::string & key, const std::string & fallback) const {
    /**
     * @brief args 是懒加载统一字段入口；调用方不需要关心字段来自顶层还是原始 args。
     */
    if (arg_overrides_) {
        if (const auto it = arg_overrides_->find(key); it != arg_overrides_->end()) return it->second;
    }
    if (args_materialized_) {
        if (args_) {
            if (const auto it = args_->find(key); it != args_->end()) return it->second;
        }
        return fallback;
    }
    std::string value;
    if (lookup_raw_arg(key, value)) return value;
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

void TraceEvent::set_arg(const std::string & key, const std::string & value) {
    if (!arg_overrides_) arg_overrides_ = std::make_unique<std::unordered_map<std::string, std::string>>();
    (*arg_overrides_)[key] = value;
    if (args_materialized_) {
        if (!args_) args_ = std::make_unique<std::unordered_map<std::string, std::string>>();
        (*args_)[key] = value;
    }
}

void TraceEvent::merge_args_from(const TraceEvent & other) {
    if (!arg_overrides_) arg_overrides_ = std::make_unique<std::unordered_map<std::string, std::string>>();
    for (const auto & item : other.args_map()) {
        (*arg_overrides_)[item.first] = item.second;
        if (args_materialized_) {
            if (!args_) args_ = std::make_unique<std::unordered_map<std::string, std::string>>();
            (*args_)[item.first] = item.second;
        }
    }
}

const std::unordered_map<std::string, std::string> & TraceEvent::args_map() const {
    ensure_args_materialized();
    return *args_;
}

void TraceEvent::ensure_args_materialized() const {
    if (args_materialized_) return;
    args_materialized_ = true;
    args_ = std::make_unique<std::unordered_map<std::string, std::string>>();
    (*args_)["pid"] = pid;
    (*args_)["tid"] = tid;
    if (event_id.empty()) {
        // no-op
    }
    else {
        (*args_)["event_id"] = event_id;
    }
    auto raw_args = args_json_view();
    if (!raw_args.empty()) {
        try {
            auto value = trace_event_detail::Json::parse(raw_args);
            if (value.is_string()) value = trace_event_detail::Json::parse(value.get<std::string>());
            trace_event_detail::flatten_args(value, "", *args_);
        }
        catch (...) {
        }
    }
    if (arg_overrides_) {
        for (const auto & item : *arg_overrides_) (*args_)[item.first] = item.second;
    }
}

bool TraceEvent::lookup_raw_arg(const std::string & key, std::string & value) const {
    if (key == "pid") {
        value = pid;
        return true;
    }
    if (key == "tid") {
        value = tid;
        return true;
    }
    if (key == "event_id" && !event_id.empty()) {
        value = event_id;
        return true;
    }

    auto raw_args = trace_event_detail::trim_view(args_json_view());
    if (raw_args.empty()) return false;

    std::string_view current = raw_args;
    if (current.front() == '"') {
        try {
            auto decoded = trace_event_detail::Json::parse(current).get<std::string>();
            auto parsed = trace_event_detail::Json::parse(decoded);
            std::unordered_map<std::string, std::string> flattened;
            trace_event_detail::flatten_args(parsed, "", flattened);
            if (const auto it = flattened.find(key); it != flattened.end()) {
                value = it->second;
                return true;
            }
            return false;
        }
        catch (...) {
            return false;
        }
    }

    std::string_view raw_value;
    if (trace_event_detail::find_top_level_key_value(current, key, raw_value)) {
        value = trace_event_detail::json_value_to_string(raw_value);
        return true;
    }

    const auto dot = key.find('.');
    if (dot != std::string::npos) {
        std::string_view parent_value;
        if (trace_event_detail::find_top_level_key_value(current, std::string_view(key).substr(0, dot), parent_value)) {
            parent_value = trace_event_detail::trim_view(parent_value);
            if (!parent_value.empty() && parent_value.front() == '"') {
                try {
                    auto decoded = trace_event_detail::Json::parse(parent_value).get<std::string>();
                    parent_value = decoded;
                    std::string_view nested_value;
                    if (trace_event_detail::find_top_level_key_value(parent_value, std::string_view(key).substr(dot + 1), nested_value)) {
                        value = trace_event_detail::json_value_to_string(nested_value);
                        return true;
                    }
                }
                catch (...) {
                }
            }
            else {
                std::string_view nested_value;
                if (trace_event_detail::find_top_level_key_value(parent_value, std::string_view(key).substr(dot + 1), nested_value)) {
                    value = trace_event_detail::json_value_to_string(nested_value);
                    return true;
                }
            }
        }
    }

    return false;
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
