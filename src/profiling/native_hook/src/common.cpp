#include "framework/common.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

namespace HookFrameWork {

std::string GetFirstNonEmptyEnv(const std::vector<std::string> & env_names) {
    for (size_t i = 0; i < env_names.size(); ++i) {
        const std::string value{ safe_env(env_names[i].c_str()) };
        if (!value.empty()) { return value; }
    }
    return "";
}

std::string GetRankString() {
    const std::vector<std::string> rank_env_names{
        "RANK_ID",
        "HOROVOD_RANK",
        "OMPI_COMM_WORLD_RANK",
    };
    const std::string rank = GetFirstNonEmptyEnv(rank_env_names);
    return rank.empty() ? "unknown" : rank;
}

std::string BuildTraceOutputPath(const std::string & rank_str, const std::string & default_path) {
    const std::string trace_base_env{ safe_env("HOOK_TRACE_OUTPUT") };
    std::string trace_base{ trace_base_env.empty() ? default_path : trace_base_env };
    return trace_base + ".rank" + rank_str + ".pid" + std::to_string(getpid()) + ".json";
}

bool ParseEnvFlag(const std::string & env_name, bool default_value) {
    const std::string value{ safe_env(env_name.c_str()) };
    if (value.empty()) { return default_value; }

    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (normalized == "1" || normalized == "true" || normalized == "on" || normalized == "yes") { return true; }
    if (normalized == "0" || normalized == "false" || normalized == "off" || normalized == "no") { return false; }
    return default_value;
}

std::string TrimCopy(const std::string & input) {
    size_t begin = 0;
    while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin]))) { ++begin; }

    size_t end = input.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1]))) { --end; }

    return input.substr(begin, end - begin);
}

std::vector<std::string> SplitCsv(const std::string & csv) {
    std::vector<std::string> tokens;
    size_t start = 0;
    while (start <= csv.size()) {
        size_t comma_pos = csv.find(',', start);
        std::string token = (comma_pos == std::string::npos) ? csv.substr(start) : csv.substr(start, comma_pos - start);
        token = TrimCopy(token);
        if (!token.empty()) { tokens.push_back(token); }
        if (comma_pos == std::string::npos) { break; }
        start = comma_pos + 1;
    }
    return tokens;
}

std::vector<std::string> SplitByChar(const std::string & input, char delimiter) {
    std::vector<std::string> tokens;
    size_t start = 0;
    while (start <= input.size()) {
        size_t pos = input.find(delimiter, start);
        std::string token = (pos == std::string::npos) ? input.substr(start) : input.substr(start, pos - start);
        token = TrimCopy(token);
        if (!token.empty()) { tokens.push_back(token); }
        if (pos == std::string::npos) { break; }
        start = pos + 1;
    }
    return tokens;
}

std::string EscapeJsonString(const std::string & input) {
    std::string output;
    output.reserve(input.size() + 16);

    for (unsigned char ch : input) {
        switch (ch) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (ch < 0x20) {
                char hex_buffer[8];
                std::snprintf(hex_buffer, sizeof(hex_buffer), "\\u%04x", static_cast<unsigned int>(ch));
                output += hex_buffer;
            }
            else { output += static_cast<char>(ch); }
        }
    }

    return output;
}

} // namespace HookFrameWork
