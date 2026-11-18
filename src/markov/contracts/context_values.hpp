#pragma once

#include <cstddef>
#include <string_view>

#include <nlohmann/json.hpp>

namespace markov {

// Measured conditions and optional traces share values only at explicit work-context boundaries.
class ContextValues {
public:
    template<class Index>
    static void encode(nlohmann::json & frame, const Index & index) {
        if (!frame.is_object()) return;
        for (auto & [name, value] : frame.items()) {
            if (snapshot(name) && value.is_object()) {
                for (auto & field : value) field = index(field);
            } else if (name == "callers" && value.is_array()) {
                for (auto & caller : value) encode(caller, index);
            } else if (nested(name)) {
                encode(value, index);
            }
        }
    }

    static bool matches(const nlohmann::json & encoded, const nlohmann::json & actual, const nlohmann::json & values) {
        if (!actual.is_object()) return encoded == actual;
        if (!encoded.is_object() || encoded.size() != actual.size()) return false;
        for (const auto & [name, field] : actual.items()) {
            const auto recorded = encoded.find(name);
            if (recorded == encoded.end()) return false;

            if (snapshot(name) && field.is_object()) {
                if (!matches_snapshot(*recorded, field, values)) return false;
            } else if (name == "callers" && field.is_array()) {
                if (!recorded->is_array() || recorded->size() != field.size()) return false;
                for (std::size_t number = 0; number < field.size(); ++number) {
                    if (!matches((*recorded)[number], field[number], values)) return false;
                }
            } else if (nested(name)) {
                if (!matches(*recorded, field, values)) return false;
            } else if (*recorded != field) {
                return false;
            }
        }
        return true;
    }

private:
    static bool snapshot(std::string_view name) {
        return name == "context" || name == "entry_context" || name == "exit_context" || name == "observation_context";
    }

    static bool nested(std::string_view name) { return name == "work" || name == "continuation"; }

    static bool matches_snapshot(const nlohmann::json & encoded, const nlohmann::json & actual, const nlohmann::json & values) {
        if (!encoded.is_object() || encoded.size() != actual.size()) return false;
        for (const auto & [name, field] : actual.items()) {
            const auto reference = encoded.find(name);
            if (reference == encoded.end() || !reference->is_number_unsigned()) return false;
            const auto index = reference->get<std::size_t>();

            // Positions only resolve values in this artifact. Identity matching still uses the complete original value.
            if (index >= values.size() || values[index] != field) return false;
        }
        return true;
    }
};

} // namespace markov
