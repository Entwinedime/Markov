/** @file Internal contracts shared by source I/O ledger construction units. */
#pragma once

#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_operation_ledger.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace markov::trace_graph::modules::hicache::patch::io_operation_ledger_detail {

[[nodiscard]] std::optional<HiCacheIoOperationKind> operation_kind(std::string_view role);
[[nodiscard]] std::string direction(HiCacheIoOperationKind kind);
[[nodiscard]] std::string_view device_trace_direction(HiCacheIoOperationKind kind);
[[nodiscard]] std::string observed_span_semantics(HiCacheIoOperationKind kind);
[[nodiscard]] uint64_t fact_end(const adapters::sglang::HiCacheFactObservation & fact);
[[nodiscard]] uint64_t fact_boundary(const adapters::sglang::HiCacheFactObservation & fact);
[[nodiscard]] const adapters::sglang::HiCacheFactObservation * paired_call_start(const adapters::sglang::HiCacheSourceIndex & source,
                                                                                 const adapters::sglang::HiCacheFactObservation & end);
[[nodiscard]] std::vector<const adapters::sglang::HiCacheFactObservation *> nested_storage_services(const adapters::sglang::HiCacheSourceIndex & source,
                                                                                                    const adapters::sglang::HiCacheFactObservation & timing,
                                                                                                    HiCacheIoOperationKind kind);
[[nodiscard]] uint64_t page_size_for_scope(const adapters::sglang::HiCacheSourceIndex & source, std::string_view cache_scope);
[[nodiscard]] uint64_t unique_source_page_size(const adapters::sglang::HiCacheSourceIndex & source);
[[nodiscard]] const adapters::sglang::HiCacheFactObservation * load_decision_for_timing(const adapters::sglang::HiCacheSourceIndex & source,
                                                                                        const adapters::sglang::HiCacheFactObservation & timing);
[[nodiscard]] const adapters::sglang::HiCacheFactObservation * request_consumer(const adapters::sglang::HiCacheSourceIndex & source,
                                                                                std::string_view request_id, std::string_view pid, uint64_t source_end_us);
[[nodiscard]] const adapters::sglang::HiCacheFactObservation * request_role_before(const adapters::sglang::HiCacheSourceIndex & source,
                                                                                   std::string_view request_id, std::string_view pid, std::string_view role,
                                                                                   uint64_t timestamp_us);
[[nodiscard]] const adapters::sglang::HiCacheFactObservation * write_consumer(const adapters::sglang::HiCacheSourceIndex & source,
                                                                              const adapters::sglang::HiCacheFactObservation & timing);
[[nodiscard]] uint64_t completed_tokens(const adapters::sglang::HiCacheFactObservation & fact);
void append_reason(HiCacheIoOperationRecord & record, std::string reason);
void build_prefetch_completion_wait_contract(const adapters::sglang::HiCacheSourceIndex & source, HiCacheIoOperationRecord & record);
void build_loadback_readiness_contract(const adapters::sglang::HiCacheSourceIndex & source, HiCacheIoOperationRecord & record,
                                       const adapters::sglang::DeviceTransferClosure & closure);
void build_device_to_host_readiness_contract(const adapters::sglang::HiCacheSourceIndex & source, HiCacheIoOperationRecord & record,
                                             const adapters::sglang::DeviceTransferClosure & closure);
void build_load_admission_control(const adapters::sglang::HiCacheSourceIndex & source, const adapters::sglang::HiCacheFactObservation & decision,
                                  HiCacheIoOperationRecord & record);
[[nodiscard]] HiCacheIoOperationRecord build_record(const adapters::sglang::HiCacheSourceIndex & source,
                                                    const adapters::sglang::HiCacheFactObservation & timing, HiCacheIoOperationKind kind);
} // namespace markov::trace_graph::modules::hicache::patch::io_operation_ledger_detail
