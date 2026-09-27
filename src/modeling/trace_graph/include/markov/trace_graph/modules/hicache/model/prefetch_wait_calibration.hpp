#pragma once
#include "markov/trace_graph/modules/hicache/model/prefetch_wait_execution.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_queue_execution.hpp"
#include <nlohmann/json_fwd.hpp>

namespace markov::trace_graph::modules::hicache::model {
struct PrefetchQueryCalibration {
    std::string source_manifest;
    PrefetchQueueTiming timing;
};
[[nodiscard]] nlohmann::json encode_prefetch_query_calibration(const PrefetchQueryCalibration & calibration);
[[nodiscard]] PrefetchQueryCalibration decode_prefetch_query_calibration(const nlohmann::json & document);
[[nodiscard]] PrefetchQueryCalibration read_prefetch_query_calibration(const std::string & path);
struct PrefetchWaitCalibration {
    std::string source_manifest, request_id, source_policy;
    PrefetchWaitTiming timing;
};
[[nodiscard]] nlohmann::json encode_prefetch_wait_calibration(const PrefetchWaitCalibration & calibration);
[[nodiscard]] PrefetchWaitCalibration decode_prefetch_wait_calibration(const nlohmann::json & document);
[[nodiscard]] PrefetchWaitCalibration read_prefetch_wait_calibration(const std::string & path);
} // namespace markov::trace_graph::modules::hicache::model
