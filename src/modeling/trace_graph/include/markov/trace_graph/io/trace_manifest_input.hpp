/**
 * @file
 * @brief In-memory channel join from `profile_manifest.json` to logical TraceGraph inputs.
 */
#pragma once

#include "markov/trace_graph/core/trace_event.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace markov::trace_graph::io {

/** @brief Concurrency, channel selection, and timestamp-association options. */
struct ManifestTraceInputOptions {
    /** @brief Total budget shared by logical-input loading and DAG construction. */
    size_t threads = 1;

    /** @brief Parser partitions used within each selected trace file. */
    size_t file_threads = 1;

    /** @brief Native-wrapper to profiler matching tolerance in microseconds. */
    double tolerance_us = 10'000.0;

    /** @brief Candidate count inspected on each side of a timestamp insertion point. */
    size_t search_window = 5;

    /** @brief Pre-profiler margin for standalone custom events, in microseconds. */
    double margin_us = 100.0;

    /** @brief Selects torch-profiler traces for this backend invocation. */
    bool include_torch = true;

    /** @brief Selects LD_PRELOAD/native traces for this backend invocation. */
    bool include_ld_preload = true;

    /** @brief Selects Python-probe sidecars for this backend invocation. */
    bool include_python_probe = true;

    /** @brief Optional inclusive lower timestamp bound in trace microseconds. */
    std::optional<uint64_t> window_start_us;

    /** @brief Optional inclusive upper timestamp bound in trace microseconds. */
    std::optional<uint64_t> window_end_us;
};

/** @brief One logical input after selected channels have been joined in memory. */
struct ManifestTraceInput {
    std::vector<core::TraceEvent> events;
    /** @brief Pre-window side-table records needed to decode in-window event arguments. */
    std::vector<core::TraceEvent> context_events;
    /** @brief Pre-window semantic facts replayed only to reconstruct initial model state. */
    std::vector<core::TraceEvent> prelude_context_events;
    /** @brief Post-window semantic facts retained only to prove causal-tail closure. */
    std::vector<core::TraceEvent> tail_context_events;
    std::vector<std::string> input_contracts;
};

/**
 * @brief Loads selected manifest channels and produces independent logical inputs.
 *
 * No merged trace file is written. Missing manifest-declared files and malformed path
 * entries fail the invocation instead of silently reducing the fact set.
 */
[[nodiscard]] std::vector<ManifestTraceInput> load_trace_inputs_from_manifest(const std::string & manifest_path, const ManifestTraceInputOptions & options);

} // namespace markov::trace_graph::io
