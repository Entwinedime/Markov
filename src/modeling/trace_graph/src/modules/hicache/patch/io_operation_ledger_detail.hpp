/** @file Internal contracts shared by source I/O ledger construction units. */
#pragma once

#include "markov/trace_graph/modules/hicache/patch/io_operation_ledger.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace markov::trace_graph::modules::hicache::patch::io_operation_ledger_detail {

[[nodiscard]] std::optional<HiCacheIoOperationKind> operation_kind(std::string_view role);
[[nodiscard]] std::string direction(HiCacheIoOperationKind kind);
[[nodiscard]] std::string_view device_trace_direction(HiCacheIoOperationKind kind);
[[nodiscard]] std::string observed_span_semantics(HiCacheIoOperationKind kind);
[[nodiscard]] uint64_t fact_end(const HiCacheSourceFactNode & fact);
[[nodiscard]] uint64_t fact_boundary(const HiCacheSourceFactNode & fact);
[[nodiscard]] const HiCacheSourceFactNode * paired_call_start(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & end);
[[nodiscard]] uint64_t page_size_for_scope(const HiCacheSourceDagIndex & source, std::string_view cache_scope);
[[nodiscard]] uint64_t unique_source_page_size(const HiCacheSourceDagIndex & source);
[[nodiscard]] const HiCacheSourceFactNode * load_decision_for_timing(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & timing);
[[nodiscard]] const HiCacheSourceFactNode * request_consumer(const HiCacheSourceDagIndex & source, std::string_view request_id,
                                                             std::string_view pid, uint64_t source_end_us);
[[nodiscard]] const HiCacheSourceFactNode * request_role_before(const HiCacheSourceDagIndex & source, std::string_view request_id,
                                                                std::string_view pid, std::string_view role, uint64_t timestamp_us);
[[nodiscard]] const HiCacheSourceFactNode * write_consumer(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & timing);
[[nodiscard]] uint64_t completed_tokens(const HiCacheSourceFactNode & fact);
void append_reason(HiCacheIoOperationRecord & record, std::string reason);
void build_prefetch_completion_wait_contract(const HiCacheSourceDagIndex & source, HiCacheIoOperationRecord & record);
void build_loadback_readiness_contract(const HiCacheSourceDagIndex & source, HiCacheIoOperationRecord & record,
                                       const HiCacheDeviceTransferClosure & closure);
void build_device_to_host_readiness_contract(const HiCacheSourceDagIndex & source, HiCacheIoOperationRecord & record,
                                             const HiCacheDeviceTransferClosure & closure);
void build_load_admission_control(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & decision, HiCacheIoOperationRecord & record);
[[nodiscard]] HiCacheIoOperationRecord build_record(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & timing,
                                                     HiCacheIoOperationKind kind);

} // namespace markov::trace_graph::modules::hicache::patch::io_operation_ledger_detail
