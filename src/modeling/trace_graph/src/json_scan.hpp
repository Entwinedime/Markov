#pragma once

#include <cstddef>
#include <string_view>

namespace markov::trace_graph::json_detail {

// Locate JSON slices without materializing them. These scanners find boundaries;
// typed consumers remain responsible for validating the selected value.
inline size_t skip_string(std::string_view text, size_t pos) {
    if (pos >= text.size() || text[pos] != '"') return pos;

    for (++pos; pos < text.size(); ++pos) {
        if (text[pos] == '\\' && pos + 1 < text.size()) ++pos;
        else if (text[pos] == '"') return pos + 1;
    }
    return text.size();
}

inline size_t skip_value(std::string_view text, size_t pos) {
    while (pos < text.size() && static_cast<unsigned char>(text[pos]) <= ' ') ++pos;
    if (pos == text.size()) return pos;
    if (text[pos] == '"') return skip_string(text, pos);

    if (text[pos] == '{' || text[pos] == '[') {
        size_t depth = 0;
        while (pos < text.size()) {
            const char value = text[pos];
            if (value == '"') {
                pos = skip_string(text, pos);
                continue;
            }

            ++pos;
            if (value == '{' || value == '[') ++depth;
            else if ((value == '}' || value == ']') && --depth == 0) return pos;
        }
        return pos;
    }

    while (pos < text.size() && text[pos] != ',' && text[pos] != '}' && text[pos] != ']' && static_cast<unsigned char>(text[pos]) > ' ') ++pos;
    return pos;
}

} // namespace markov::trace_graph::json_detail
