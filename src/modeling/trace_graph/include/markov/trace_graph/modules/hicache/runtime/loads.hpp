#pragma once
#include "markov/trace_graph/modules/hicache/model/replay.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_operation_ledger.hpp"
#include "markov/trace_graph/modules/hicache/patch/layer_io.hpp"
#include "markov/trace_graph/modules/hicache/runtime/load_consumers.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <functional>
#include <set>

namespace markov::trace_graph::modules::hicache::runtime {

enum class LoadExecutionStage { Submission, DeviceStart, DeviceComplete };
struct LoadExecutionEvent {
    LoadExecutionStage stage;
    std::string pid;
    uint64_t timestamp_us;
    size_t operations = 0;
    uint64_t bytes = 0, service_us = 0, source_transfer_us = 0;
};

/** Controller-scoped queue submission and target H2D execution at source load
 * sites. With independent submission costs, target state supplies batches and
 * page counts; the backend program generates copies and layer Records. Source
 * calls supply execution sites and resource evidence, not target cardinality.
 * Device completion does not acknowledge the batch or release locks.
 * Bind before ordinary CPU/control rewrites and rebind workers after removal.
 * Source-empty loads expand their submission and supply new layer Records to
 * the dynamic layer-call consumer. Dependencies on other batches remain an
 * explicit unsupported path rather than silently zero cost.
 */
class HiCacheLoads {
public:
    explicit HiCacheLoads(HiCacheWriteStreamInsertions & insertions, const frontend::HiCacheConfig & config = {})
        : page_bytes_(config.kv_bytes_per_page), submission_calibration_(config.load_submission_calibration), stream_insertions_(insertions) {}
    using Observer = std::function<void(const LoadExecutionEvent &)>;
    void bind(core::DagGraph & graph, model::HiCacheModelReplay & replay, uint64_t begin_us, uint64_t end_us, Observer observer = {});
    // Layer consumers must first detach from the old completion records.
    void replace_source_submissions(core::DagGraph & graph);
    void advance(size_t node, uint64_t absolute_time_us, simulation::FutureDag & future);
    void rebind_workers(const core::DagGraph & graph, const std::map<size_t, size_t> & members);
    [[nodiscard]] size_t source_submissions() const { return loads_.size(); }
    [[nodiscard]] size_t completed_batches() const { return completed_; }
    [[nodiscard]] const std::vector<size_t> & layer_consumer(const std::string & pid, uint64_t batch_start_ns) const {
        return forward_records_.at({ pid, batch_start_ns });
    }

private:
    struct Donor {
        std::string pid, tid;
        HiCacheWriteExpansion plan;
        std::vector<size_t> records, lanes;
    };
    struct Insertion {
        HiCacheHostExpansion empty;
        size_t continuation;
        std::map<size_t, HiCacheWriteStreamPosition> positions;
    };
    void prepare_insertions(core::DagGraph & graph, std::span<const std::optional<size_t>> boundaries);
    void prepare_generated_submissions(core::DagGraph & graph);
    struct GeneratedSubmission {
        size_t main, worker, compute, stream, layers;
        LoadSubmissionCost cost;
    };
    std::map<std::pair<std::string, std::string>, GeneratedSubmission> generated_submissions_;
    core::DagGraph * graph_ = nullptr;
    uint64_t page_bytes_ = 0;
    std::string submission_calibration_;
    std::vector<Donor> donors_;
    std::map<size_t, Insertion> insertions_;
    std::vector<std::optional<size_t>> replacement_boundaries_;
    HiCacheWriteStreamInsertions & stream_insertions_;
    struct Load {
        HiCacheFact fact;
        patch::HiCacheIoOperationRecord record;
        std::vector<patch::HiCacheLayerTransferTemplate> layers;
        std::optional<model::HiCacheLoadbackBatch> batch;
    };
    std::vector<Load> loads_;
    struct Forward {
        std::string pid, request;
        uint64_t start_ns;
        bool extend;
    };
    std::vector<Forward> forwards_;
    std::map<size_t, std::vector<size_t>> forward_at_;
    HiCacheLoadConsumers consumers_;
    std::map<std::pair<std::string, uint64_t>, std::vector<size_t>> forward_records_;
    std::map<size_t, size_t> submit_at_, start_at_, complete_at_;
    model::HiCacheState * state_ = nullptr;
    Observer observer_;
    size_t completed_ = 0;
};

} // namespace markov::trace_graph::modules::hicache::runtime
