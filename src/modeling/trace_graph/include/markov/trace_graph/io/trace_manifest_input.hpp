/**
 * @file
 * @brief profile_manifest.json 到 C++ TraceGraph 输入事件的内存合流层。
 */
#pragma once

#include "markov/trace_graph/core/trace_event.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace markov::trace_graph::io {

/** @brief C++ manifest reader 的并发和匹配选项。 */
struct ManifestTraceInputOptions {
    /** @brief logical input 之间的读取/构图并发上限。 */
    size_t threads = 1;

    /** @brief 单 trace 文件内部 event object 解析线程数。 */
    size_t file_threads = 1;

    /** @brief LD_PRELOAD wrapper 与 torch profiler event 的 timestamp 容忍窗口，单位 us。 */
    double tolerance_us = 10'000.0;

    /** @brief search 模式下每侧候选窗口大小。 */
    size_t search_window = 5;

    /** @brief earliest profiler timestamp 之前仍保留 standalone custom event 的余量，单位 us。 */
    double margin_us = 100.0;

    /** @brief 当前支持 search；sequential 保留为后续严格审计扩展点。 */
    std::string mode = "search";

    /** @brief 是否允许 torch profiler trace 进入 C++ 后端。 */
    bool include_torch = true;

    /** @brief 是否允许 LD_PRELOAD/native trace 进入 C++ 后端。 */
    bool include_ld_preload = true;

    /** @brief 是否允许 Python probe sidecar trace 进入 C++ 后端。 */
    bool include_python_probe = true;
};

/** @brief 一组 torch / LD_PRELOAD / Python probe 合流后的 logical trace input。 */
struct ManifestTraceInput {
    std::vector<core::TraceEvent> events;
    std::vector<std::string> source_paths;
    std::string torch_path;
    std::string ld_preload_path;
    std::vector<std::string> sidecar_paths;
};

/** @brief 读取 profile_manifest.json 并在内存中执行 trace merger 语义。 */
[[nodiscard]] std::vector<ManifestTraceInput> load_trace_inputs_from_manifest(const std::string & manifest_path,
                                                                              const ManifestTraceInputOptions & options);

/** @brief 仅展开 manifest 中真实存在的 trace 文件，供 Python validation artifact 记录原始输入。 */
[[nodiscard]] std::vector<std::string> trace_paths_from_manifest(const std::string & manifest_path);

} // namespace markov::trace_graph::io
