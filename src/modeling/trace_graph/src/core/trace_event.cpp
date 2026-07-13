/**
 * @file
 * @brief Lazy TraceEvent argument access and streaming JSON escaping.
 */
#include "markov/trace_graph/core/trace_event.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <charconv>
#include <ranges>
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

void flatten_args(const Json & value, const std::string & prefix, TraceArgMap & args) {
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

bool json_key_equals(std::string_view literal, std::string_view expected) {
    literal = trim_view(literal);
    if (literal.size() < 2 || literal.front() != '"' || literal.back() != '"') return false;
    const auto content = literal.substr(1, literal.size() - 2);
    if (content.find('\\') == std::string_view::npos) return content == expected;
    return decode_json_string(literal) == expected;
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

size_t skip_json_value(std::string_view text, size_t pos);

size_t skip_json_compound(std::string_view text, size_t pos) {
    const char close = text[pos] == '{' ? '}' : ']';
    ++pos;
    while (pos < text.size()) {
        if (text[pos] == '"') {
            pos = skip_json_string(text, pos);
            continue;
        }
        if (text[pos] == '{' || text[pos] == '[') {
            pos = skip_json_compound(text, pos);
            continue;
        }
        if (text[pos] == close) return pos + 1;
        ++pos;
    }
    return text.size();
}

size_t skip_json_primitive(std::string_view text, size_t pos) {
    while (pos < text.size() && text[pos] != ',' && text[pos] != '}' && text[pos] != ']' && static_cast<unsigned char>(text[pos]) > ' ') ++pos;
    return pos;
}

size_t skip_json_value(std::string_view text, size_t pos) {
    while (pos < text.size() && static_cast<unsigned char>(text[pos]) <= ' ') ++pos;
    if (pos >= text.size()) return pos;
    if (text[pos] == '"') return skip_json_string(text, pos);
    if (text[pos] == '{' || text[pos] == '[') return skip_json_compound(text, pos);
    return skip_json_primitive(text, pos);
}

class TopLevelObjectLookup {
public:
    explicit TopLevelObjectLookup(std::string_view object) : object_(trim_view(object)), pos_(object_.empty() ? 0 : 1) {}

    [[nodiscard]] bool find(std::string_view expected_key, std::string_view & value) {
        if (object_.empty() || object_.front() != '{') return false;
        while (prepare_next_member()) {
            if (object_[pos_] != '"') {
                skip_unknown_member_fragment();
                continue;
            }
            const auto key_literal = parse_key_literal();
            if (!consume_colon()) return false;
            const auto member_value = parse_value_literal();
            if (json_key_equals(key_literal, expected_key)) {
                value = member_value;
                return true;
            }
            consume_comma();
        }
        return false;
    }

private:
    void skip_ws() {
        while (pos_ < object_.size() && static_cast<unsigned char>(object_[pos_]) <= ' ') ++pos_;
    }

    [[nodiscard]] bool prepare_next_member() {
        skip_ws();
        return pos_ < object_.size() && object_[pos_] != '}';
    }

    void skip_unknown_member_fragment() {
        pos_ = skip_json_value(object_, pos_);
        consume_comma();
    }

    [[nodiscard]] std::string_view parse_key_literal() {
        const auto begin = pos_;
        pos_ = skip_json_string(object_, pos_);
        return object_.substr(begin, pos_ - begin);
    }

    [[nodiscard]] bool consume_colon() {
        skip_ws();
        if (pos_ >= object_.size() || object_[pos_] != ':') return false;
        ++pos_;
        skip_ws();
        return true;
    }

    [[nodiscard]] std::string_view parse_value_literal() {
        const auto begin = pos_;
        pos_ = skip_json_value(object_, pos_);
        return object_.substr(begin, pos_ - begin);
    }

    void consume_comma() {
        skip_ws();
        if (pos_ < object_.size() && object_[pos_] == ',') ++pos_;
    }

    std::string_view object_;
    size_t pos_ = 0;
};

bool find_top_level_key_value(std::string_view object, std::string_view key, std::string_view & value) { return TopLevelObjectLookup(object).find(key, value); }

std::string json_value_to_string(std::string_view value) {
    value = trim_view(value);
    if (value.empty() || value == "null") return "";
    if (value.front() == '"') return decode_json_string(value);
    return std::string(value);
}

void materialize_json_args(std::string_view raw_args, TraceArgMap & args) {
    raw_args = trim_view(raw_args);
    if (raw_args.empty()) return;
    try {
        auto value = Json::parse(raw_args);
        if (value.is_string()) value = Json::parse(value.get<std::string>());
        flatten_args(value, "", args);
    }
    catch (const Json::exception &) {
        return;
    }
}

bool assign_json_value(std::string_view raw_value, std::string * value) {
    if (value != nullptr) *value = json_value_to_string(raw_value);
    return true;
}

bool lookup_encoded_json_arg(std::string_view encoded_args, std::string_view key, std::string * value) {
    try {
        auto decoded = Json::parse(encoded_args).get<std::string>();
        auto parsed = Json::parse(decoded);
        TraceArgMap flattened;
        flatten_args(parsed, "", flattened);
        const auto found = flattened.find(key);
        if (found == flattened.end()) return false;
        if (value != nullptr) *value = found->second;
        return true;
    }
    catch (const Json::exception &) {
        return false;
    }
}

bool lookup_nested_json_arg(std::string_view object, std::string_view key, std::string * value) {
    const auto dot = key.find('.');
    if (dot == std::string::npos) return false;

    std::string_view parent_value;
    if (!find_top_level_key_value(object, key.substr(0, dot), parent_value)) return false;
    parent_value = trim_view(parent_value);
    const auto child_key = key.substr(dot + 1);
    if (!parent_value.empty() && parent_value.front() == '"') {
        try {
            const auto decoded = Json::parse(parent_value).get<std::string>();
            std::string_view nested_value;
            return find_top_level_key_value(decoded, child_key, nested_value) && assign_json_value(nested_value, value);
        }
        catch (const Json::exception &) {
            return false;
        }
    }

    std::string_view nested_value;
    return find_top_level_key_value(parent_value, child_key, nested_value) && assign_json_value(nested_value, value);
}

bool lookup_json_arg(std::string_view raw_args, std::string_view key, std::string * value) {
    const auto current = trim_view(raw_args);
    if (current.empty()) return false;
    if (current.front() == '"') return lookup_encoded_json_arg(current, key, value);
    std::string_view raw_value;
    if (find_top_level_key_value(current, key, raw_value)) return assign_json_value(raw_value, value);
    return lookup_nested_json_arg(current, key, value);
}

} // namespace trace_event_detail

TraceEvent::TraceEvent(const TraceEvent & other)
    : index(other.index),
      source_channel(other.source_channel),
      event_id(other.event_id),
      name(other.name),
      cat(other.cat),
      ph(other.ph),
      ts(other.ts),
      dur(other.dur),
      pid(other.pid),
      tid(other.tid),
      args_materialized_(other.args_materialized_),
      args_buffer_(other.args_buffer_),
      args_offset_(other.args_offset_),
      args_length_(other.args_length_) {
    if (other.owned_args_json_) owned_args_json_ = std::make_unique<std::string>(*other.owned_args_json_);
    if (other.args_) args_ = std::make_unique<TraceArgMap>(*other.args_);
    if (other.arg_overrides_) arg_overrides_ = std::make_unique<TraceArgMap>(*other.arg_overrides_);
    if (other.arg_layers_) arg_layers_ = std::make_unique<std::vector<TraceArgLayer>>(*other.arg_layers_);
}

TraceEvent & TraceEvent::operator=(const TraceEvent & other) {
    if (this == &other) return *this;
    index = other.index;
    source_channel = other.source_channel;
    event_id = other.event_id;
    name = other.name;
    cat = other.cat;
    ph = other.ph;
    ts = other.ts;
    dur = other.dur;
    pid = other.pid;
    tid = other.tid;
    args_materialized_ = other.args_materialized_;
    owned_args_json_ = other.owned_args_json_ ? std::make_unique<std::string>(*other.owned_args_json_) : nullptr;
    args_buffer_ = other.args_buffer_;
    args_offset_ = other.args_offset_;
    args_length_ = other.args_length_;
    args_.reset();
    arg_overrides_.reset();
    arg_layers_.reset();
    if (other.args_) args_ = std::make_unique<TraceArgMap>(*other.args_);
    if (other.arg_overrides_) arg_overrides_ = std::make_unique<TraceArgMap>(*other.arg_overrides_);
    if (other.arg_layers_) arg_layers_ = std::make_unique<std::vector<TraceArgLayer>>(*other.arg_layers_);
    return *this;
}

void TraceEvent::set_args_json_slice(std::shared_ptr<const std::string> buffer, const TraceByteRange & range) {
    args_buffer_ = std::move(buffer);
    args_offset_ = range.offset;
    args_length_ = range.length;
    owned_args_json_.reset();
    args_materialized_ = false;
    args_.reset();
    arg_layers_.reset();
}

std::string_view TraceEvent::args_json_view() const {
    if (args_buffer_ && args_offset_ <= args_buffer_->size() && args_length_ <= args_buffer_->size() - args_offset_) {
        return std::string_view(args_buffer_->data() + args_offset_, args_length_);
    }
    return owned_args_json_ ? std::string_view(*owned_args_json_) : std::string_view{};
}

bool TraceEvent::has_arg(std::string_view key) const {
    if (arg_overrides_ && arg_overrides_->contains(key)) return true;
    if (args_materialized_) return args_ && args_->contains(key);
    if (lookup_arg_layers(key, nullptr)) return true;
    return lookup_raw_arg(key, nullptr);
}

bool TraceEvent::has_arg_key_hint(std::string_view key) const {
    if (arg_overrides_ && arg_overrides_->contains(key)) return true;
    if (args_materialized_) return args_ && args_->contains(key);
    if (args_json_view().find(key) != std::string_view::npos) return true;
    if (!arg_layers_) return false;
    for (const auto & layer : *arg_layers_) {
        if (layer.overrides && layer.overrides->contains(key)) return true;
        if (layer.buffer && layer.offset <= layer.buffer->size() && layer.length <= layer.buffer->size() - layer.offset
            && std::string_view(layer.buffer->data() + layer.offset, layer.length).find(key) != std::string_view::npos)
            return true;
    }
    return false;
}

bool TraceEvent::has_arg_override(std::string_view key) const {
    if (arg_overrides_ && arg_overrides_->contains(key)) return true;
    if (!arg_layers_) return false;
    return std::ranges::any_of(*arg_layers_, [&](const auto & layer) { return layer.overrides && layer.overrides->contains(key); });
}

std::string TraceEvent::arg(std::string_view key, std::string_view fallback) const {
    auto value = find_arg(key);
    return value ? std::move(*value) : std::string(fallback);
}

std::optional<std::string> TraceEvent::find_arg(std::string_view key) const {
    // Overrides, materialized args, and raw slices form one logical argument view.
    if (arg_overrides_) {
        if (const auto it = arg_overrides_->find(key); it != arg_overrides_->end()) return it->second;
    }
    if (args_materialized_) {
        if (args_) {
            if (const auto it = args_->find(key); it != args_->end()) return it->second;
        }
        return std::nullopt;
    }
    std::string value;
    if (lookup_arg_layers(key, &value)) return value;
    if (lookup_raw_arg(key, &value)) return value;
    return std::nullopt;
}

uint64_t TraceEvent::arg_u64(std::string_view key, uint64_t fallback) const {
    const auto text = arg(key);
    const auto value = parse_u64(text);
    return value.value_or(fallback);
}

void TraceEvent::set_arg(std::string_view key, std::string_view value) {
    if (!arg_overrides_) arg_overrides_ = std::make_unique<TraceArgMap>();
    arg_overrides_->insert_or_assign(std::string(key), std::string(value));
    if (args_materialized_) {
        if (!args_) args_ = std::make_unique<TraceArgMap>();
        args_->insert_or_assign(std::string(key), std::string(value));
    }
}

void TraceEvent::freeze_arg_overrides() {
    if (!arg_overrides_ || arg_overrides_->empty()) {
        arg_overrides_.reset();
        return;
    }
    if (!arg_layers_) arg_layers_ = std::make_unique<std::vector<TraceArgLayer>>();
    arg_layers_->push_back(TraceArgLayer{
        .overrides = std::make_shared<const TraceArgMap>(std::move(*arg_overrides_)),
    });
    arg_overrides_.reset();
}

void TraceEvent::append_arg_layers_from(const TraceEvent & other) {
    if (!arg_layers_) arg_layers_ = std::make_unique<std::vector<TraceArgLayer>>();

    const auto raw = other.args_json_view();
    if (!raw.empty()) {
        TraceArgLayer base;
        if (other.args_buffer_) {
            base.buffer = other.args_buffer_;
            base.offset = other.args_offset_;
            base.length = other.args_length_;
        }
        else {
            base.buffer = std::make_shared<const std::string>(raw);
            base.length = raw.size();
        }
        arg_layers_->push_back(std::move(base));
    }
    if (other.arg_layers_) arg_layers_->insert(arg_layers_->end(), other.arg_layers_->begin(), other.arg_layers_->end());
    if (other.arg_overrides_ && !other.arg_overrides_->empty()) {
        arg_layers_->push_back(TraceArgLayer{
            .overrides = std::make_shared<const TraceArgMap>(*other.arg_overrides_),
        });
    }
}

void TraceEvent::merge_args_from(const TraceEvent & other) {
    if (!args_materialized_) {
        freeze_arg_overrides();
        append_arg_layers_from(other);
        return;
    }

    TraceArgMap merged;
    trace_event_detail::materialize_json_args(other.args_json_view(), merged);
    if (other.arg_layers_) {
        for (const auto & layer : *other.arg_layers_) {
            if (layer.buffer && layer.offset <= layer.buffer->size() && layer.length <= layer.buffer->size() - layer.offset) {
                trace_event_detail::materialize_json_args(std::string_view(layer.buffer->data() + layer.offset, layer.length), merged);
            }
            if (layer.overrides) {
                for (const auto & item : *layer.overrides) merged[item.first] = item.second;
            }
        }
    }
    if (other.arg_overrides_) {
        for (const auto & item : *other.arg_overrides_) merged[item.first] = item.second;
    }

    if (!arg_overrides_) arg_overrides_ = std::make_unique<TraceArgMap>();
    if (!args_) args_ = std::make_unique<TraceArgMap>();
    for (const auto & item : merged) {
        (*arg_overrides_)[item.first] = item.second;
        (*args_)[item.first] = item.second;
    }
}

const TraceArgMap & TraceEvent::args_map() const {
    ensure_args_materialized();
    return *args_;
}

void TraceEvent::ensure_args_materialized() const {
    if (args_materialized_) return;
    args_materialized_ = true;
    args_ = std::make_unique<TraceArgMap>();
    (*args_)["pid"] = pid;
    (*args_)["tid"] = tid;
    if (!event_id.empty()) (*args_)["event_id"] = event_id;
    trace_event_detail::materialize_json_args(args_json_view(), *args_);
    if (arg_layers_) {
        for (const auto & layer : *arg_layers_) {
            if (layer.buffer && layer.offset <= layer.buffer->size() && layer.length <= layer.buffer->size() - layer.offset) {
                trace_event_detail::materialize_json_args(std::string_view(layer.buffer->data() + layer.offset, layer.length), *args_);
            }
            if (layer.overrides) {
                for (const auto & item : *layer.overrides) (*args_)[item.first] = item.second;
            }
        }
    }
    if (arg_overrides_) {
        for (const auto & item : *arg_overrides_) (*args_)[item.first] = item.second;
    }
}

bool TraceEvent::lookup_arg_layers(std::string_view key, std::string * value) const {
    if (!arg_layers_) return false;
    for (auto layer = arg_layers_->rbegin(); layer != arg_layers_->rend(); ++layer) {
        if (layer->overrides) {
            if (const auto found = layer->overrides->find(key); found != layer->overrides->end()) {
                if (value != nullptr) *value = found->second;
                return true;
            }
        }
        if (layer->buffer && layer->offset <= layer->buffer->size() && layer->length <= layer->buffer->size() - layer->offset
            && trace_event_detail::lookup_json_arg(std::string_view(layer->buffer->data() + layer->offset, layer->length), key, value)) {
            return true;
        }
    }
    return false;
}

bool TraceEvent::lookup_raw_arg(std::string_view key, std::string * value) const {
    if (key == "pid") {
        if (value != nullptr) *value = pid;
        return true;
    }
    if (key == "tid") {
        if (value != nullptr) *value = tid;
        return true;
    }
    if (key == "event_id" && !event_id.empty()) {
        if (value != nullptr) *value = event_id;
        return true;
    }

    return trace_event_detail::lookup_json_arg(args_json_view(), key, value);
}

std::string escape_json(std::string_view input) {
    // Chrome trace needs standard control-character escaping; non-ASCII bytes pass through.
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
