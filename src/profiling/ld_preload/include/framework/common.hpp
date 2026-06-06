#pragma once

#include <string>
#include <vector>

namespace HookFrameWork {

inline std::string safe_env(const char * name, const std::string & fallback = "") {
    auto val = std::getenv(name);
    return val ? val : fallback;
}

std::string GetFirstNonEmptyEnv(const std::vector<std::string> & env_names);

std::string GetRankString();

std::string BuildTraceOutputPath(const std::string & rank_str, const std::string & default_path);

bool ParseEnvFlag(const std::string & env_name, bool default_value);

std::string TrimCopy(const std::string & input);

std::vector<std::string> SplitCsv(const std::string & csv);

std::vector<std::string> SplitByChar(const std::string & input, char delimiter);

std::string EscapeJsonString(const std::string & input);

} // namespace HookFrameWork
