/**
 * @file
 * @brief Chrome trace JSON 的读写 adapter。
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/core/trace_event.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace markov::trace_graph::io {

/** @brief Chrome trace reader 的执行选项。 */
struct TraceReadOptions {
    /** @brief 读取 metadata event；manifest 合流需要 process_name=CANN 来匹配 LD_PRELOAD 参数。 */
    bool include_metadata = false;

    /** @brief 是否保留 validation-only duration event；默认不进入 C++ 建模 DAG。 */
    bool include_validation_only = false;

    /** @brief 是否保留被 faithful replay 明确忽略的 duration event。 */
    bool include_ignored_duration = false;

    /** @brief 修复流式 trace 常见的尾部未闭合。 */
    bool auto_repair = false;

    /** @brief 单文件 JSON event object 解析线程数；1 表示串行 scanner。 */
    size_t threads = 1;
};

/**
 * @brief 读取 Chrome trace duration event。
 *
 * 当前 reader 为了处理千万级 trace 使用轻量 scanner，只抽取构图需要的字段，
 * 不保留完整 JSON DOM。
 */
[[nodiscard]] std::vector<core::TraceEvent> read_chrome_trace(const std::string & filename);

/** @brief 带选项读取 Chrome trace event。 */
[[nodiscard]] std::vector<core::TraceEvent> read_chrome_trace(const std::string & filename, const TraceReadOptions & options);

/**
 * @brief 以 Chrome trace 格式导出仿真后的 DAG。
 */
void write_chrome_trace_dag(const std::string & filename, const core::DagGraph & graph);

} // namespace markov::trace_graph::io
