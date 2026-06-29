/**
 * @file
 * @brief 归一化 Chrome trace event 的内存表示和 JSON 工具。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace markov::trace_graph::core {

/**
 * @brief C++ 后端内部统一使用的事件表示。
 *
 * 输入 Chrome trace 可能来自 torch profiler、LD_PRELOAD、Python probe 或 trace merger。
 * 这些来源的字段形态并不一致，所以 reader 会把顶层字段和 args 中可用的标量
 * 统一收敛到该结构。后续 DagBuilder 不再直接接触原始 JSON，只依赖 TraceEvent。
 */
struct TraceEvent {
    /** @brief 当前 trace 文件内的稳定顺序编号；排序后仍作为同 timestamp 的 tie breaker。 */
    size_t index = 0;

    /** @brief 可选的外部事件 id；大多数 Chrome trace duration event 不提供该字段。 */
    std::string event_id;

    /** @brief Chrome trace 顶层字段。 */
    std::string name;
    std::string cat;
    std::string ph = "X";
    uint64_t ts = 0;
    uint64_t dur = 0;
    std::string pid = "-1";
    std::string tid = "-1";

    /**
     * @brief 统一后的扩展参数。
     *
     * 为了避免 C++ 建模层依赖 nlohmann::json DOM，当前只保留字符串值。
     * 需要结构化语义的字段必须在 trace merger / normalizer 阶段提前展开成稳定 key。
     */
    std::unordered_map<std::string, std::string> args;

    /** @brief 判断统一参数表中是否存在指定 key。 */
    [[nodiscard]] bool has_arg(const std::string & key) const;
    /** @brief 读取字符串参数，缺失时返回调用方提供的 fallback。 */
    [[nodiscard]] std::string arg(const std::string & key, const std::string & fallback = "") const;
    /** @brief 以 uint64_t 形式读取参数；非法或负值输入按缺失处理。 */
    [[nodiscard]] uint64_t arg_u64(const std::string & key, uint64_t fallback = 0) const;
};

/**
 * @brief 手写 Chrome trace 输出时使用的最小 JSON 字符串转义工具。
 *
 * 这里不引入 JSON DOM，是为了避免输出超大 DAG trace 时额外复制大对象。
 */
std::string escape_json(const std::string & input);

} // namespace markov::trace_graph::core
