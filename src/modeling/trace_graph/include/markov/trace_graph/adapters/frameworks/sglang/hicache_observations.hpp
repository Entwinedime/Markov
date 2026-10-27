#pragma once

#include "markov/trace_graph/modules/hicache/fact.hpp"
#include "markov/trace_graph/source_dag/index.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::adapters::sglang {

/** @brief Lightweight semantic fact retained for source-attribution evidence. */
struct HiCacheFactObservation {
    /** @brief Semantic fact identity from the canonical HiCache fact side-table. */
    size_t node_id = 0;
    size_t event_index = 0;
    uint64_t timestamp_us = 0;
    uint64_t duration_us = 0;
    std::string pid;
    std::string tid;
    std::string target_id;
    std::string phase;
    std::string fact_class;
    std::string fact_role;
    std::string request_id;
    std::vector<std::string> batch_request_ids;
    std::string operation_id;
    std::string cache_scope;
    std::optional<uint64_t> object_node_id = std::nullopt;
    modules::hicache::HiCacheTokenSpan full_path_span;
    uint64_t source_page_size = 0;
    uint64_t service_item_count = 0;
    std::optional<uint64_t> storage_existing_page_count;
    std::optional<uint64_t> storage_new_page_count;
    uint64_t token_count = 0;
    uint64_t effective_token_count = 0;
    uint64_t completed_token_count = 0;
    bool completed_token_count_present = false;
    std::optional<bool> progress_ready = std::nullopt;
    std::optional<uint64_t> host_available_tokens_at_return;
    std::optional<bool> write_back = std::nullopt;
    std::vector<uint64_t> operation_node_ids;
    std::vector<std::string> page_hashes;
};

/** @brief Device-transfer nodes submitted by one observed host call and their existing readiness joins. */
struct DeviceTransferClosure {
    std::string status = "unresolved";
    std::vector<size_t> transfer_node_ids;
    std::vector<size_t> completion_node_ids;
    std::vector<size_t> readiness_join_node_ids;
    uint64_t transfer_duration_us = 0;
    std::string reason;
};

/** HiCache facts and control envelopes over the common immutable source index.
 * Only this extension parses cache facts. Compute and communication observers
 * accept SourceDagIndex and do not require these semantic side tables. */
class HiCacheSourceIndex : public source_dag::SourceDagIndex {
public:
    explicit HiCacheSourceIndex(const core::DagGraph & graph);
    using source_dag::SourceDagIndex::timing_interval_ownership;

    [[nodiscard]] const std::vector<HiCacheFactObservation> & fact_nodes() const { return fact_nodes_; }
    [[nodiscard]] const HiCacheFactObservation * fact_node(size_t node_id) const;
    [[nodiscard]] std::span<const HiCacheFactObservation> tail_context_facts() const { return tail_context_facts_; }
    [[nodiscard]] std::span<const size_t> nodes_for_fact_role(std::string_view role) const;
    [[nodiscard]] std::span<const size_t> nodes_for_request(std::string_view request_id) const;

    [[nodiscard]] source_dag::TimingIntervalOwnership timing_interval_ownership(const HiCacheFactObservation & fact) const {
        return timing_interval_ownership(fact.pid, fact.tid, fact.timestamp_us, fact.duration_us);
    }
    /** Smallest explicit control interval enclosing a fact. The builder retains
     * .self leaves with parent identities, not an executable envelope. The
     * consumer decides which leaves are service and which gaps remain residual. */
    [[nodiscard]] std::optional<source_dag::TimingIntervalOwnership> enclosing_control_interval_ownership(const HiCacheFactObservation & fact,
                                                                                                          std::string_view control_event_name) const;
    [[nodiscard]] DeviceTransferClosure device_transfer_closure(const HiCacheFactObservation & submission, std::string_view direction) const;

private:
    struct ControlInterval {
        std::string pid;
        std::string tid;
        uint64_t start_us = 0;
        uint64_t end_us = 0;
    };
    using ControlIntervalMap = std::unordered_map<std::string, std::vector<ControlInterval>, core::TraceArgHash, std::equal_to<>>;

    std::vector<HiCacheFactObservation> fact_nodes_, tail_context_facts_;
    std::unordered_map<size_t, size_t> fact_index_by_node_, tail_fact_index_by_node_;
    NodeMap nodes_by_fact_role_, nodes_by_request_, device_nodes_by_direction_;
    ControlIntervalMap control_intervals_by_name_;
};

} // namespace markov::trace_graph::adapters::sglang
