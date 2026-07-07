/**
 * @file
 * @brief 归一化 Chrome trace event 的内存表示和 JSON 工具。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace markov::trace_graph::core {

/**
 * @brief C++ 后端内部统一使用的事件表示。
 *
 * 输入 Chrome trace 可能来自 torch profiler、LD_PRELOAD 或 Python probe。
 * reader 只立即解析顶层字段；args 保留原始 JSON，在模型首次查询具体 key 时懒加载。
 */
struct TraceEvent {
    TraceEvent() = default;
    TraceEvent(const TraceEvent & other);
    TraceEvent & operator=(const TraceEvent & other);
    TraceEvent(TraceEvent &&) noexcept = default;
    TraceEvent & operator=(TraceEvent &&) noexcept = default;

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

    /** @brief 原始 Chrome trace args JSON；按需 materialize，不在 reader 阶段全量解析。 */
    std::string args_json;

    /** @brief 将 args 指向 reader 持有的 trace 文件 buffer，避免每个 event 复制 args 原文。 */
    void set_args_json_slice(std::shared_ptr<const std::string> buffer, size_t offset, size_t length);

    /** @brief 读取原始 args JSON 视图；仅用于轻量 marker 检查或 lazy parser。 */
    [[nodiscard]] std::string_view args_json_view() const;

    /** @brief 判断统一参数表中是否存在指定 key。 */
    [[nodiscard]] bool has_arg(const std::string & key) const;

    /** @brief 读取字符串参数，缺失时返回调用方提供的 fallback。 */
    [[nodiscard]] std::string arg(const std::string & key, const std::string & fallback = "") const;

    /** @brief 以 uint64_t 形式读取参数；非法或负值输入按缺失处理。 */
    [[nodiscard]] uint64_t arg_u64(const std::string & key, uint64_t fallback = 0) const;

    /** @brief 写入或覆盖一个合成参数；不触发原始 args materialize。 */
    void set_arg(const std::string & key, const std::string & value);

    /** @brief 合并另一个事件的 materialized args；用于 torch/LD wrapper in-memory 合流。 */
    void merge_args_from(const TraceEvent & other);

    /** @brief 只读访问 materialized args；调用方必须只在确有需要时使用。 */
    [[nodiscard]] const std::unordered_map<std::string, std::string> & args_map() const;
private:
    void ensure_args_materialized() const;
    [[nodiscard]] bool lookup_raw_arg(const std::string & key, std::string & value) const;

    mutable bool args_materialized_ = false;
    std::shared_ptr<const std::string> args_buffer_;
    size_t args_offset_ = 0;
    size_t args_length_ = 0;
    mutable std::unique_ptr<std::unordered_map<std::string, std::string>> args_;
    mutable std::unique_ptr<std::unordered_map<std::string, std::string>> arg_overrides_;
};

/**
 * @brief 手写 Chrome trace 输出时使用的最小 JSON 字符串转义工具。
 *
 * 这里不引入 JSON DOM，是为了避免输出超大 DAG trace 时额外复制大对象。
 */
std::string escape_json(const std::string & input);

} // namespace markov::trace_graph::core
