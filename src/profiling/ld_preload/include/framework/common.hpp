#pragma once

#include <string>
#include <vector>

namespace HookFrameWork {

/** @brief 读取环境变量；变量不存在时返回 fallback。 */
inline std::string safe_env(const char * name, const std::string & fallback = "") {
    auto val = std::getenv(name);
    return val ? val : fallback;
}

/** @brief 按顺序读取第一个非空环境变量值。 */
std::string GetFirstNonEmptyEnv(const std::vector<std::string> & env_names);

/** @brief 读取当前分布式 rank 字符串；缺失时返回 unknown。 */
std::string GetRankString();

/** @brief 构造带 rank 和 pid 后缀的 trace 输出路径，避免多进程写同一文件。 */
std::string BuildTraceOutputPath(const std::string & rank_str, const std::string & default_path);

/** @brief 解析常见布尔环境变量写法；非法值保持 default_value。 */
bool ParseEnvFlag(const std::string & env_name, bool default_value);

/** @brief 返回去除首尾空白的新字符串。 */
std::string TrimCopy(const std::string & input);

/** @brief 按逗号拆分并去除空 token，用于环境变量列表。 */
std::vector<std::string> SplitCsv(const std::string & csv);

/** @brief 按指定字符拆分并去除空 token。 */
std::vector<std::string> SplitByChar(const std::string & input, char delimiter);

/** @brief 输出 Chrome trace JSON 字段时使用的最小字符串转义。 */
std::string EscapeJsonString(const std::string & input);

} // namespace HookFrameWork
