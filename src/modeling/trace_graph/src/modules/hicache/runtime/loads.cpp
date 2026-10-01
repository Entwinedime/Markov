#include "markov/trace_graph/modules/hicache/runtime/loads.hpp"
#include "markov/trace_graph/core/cpu_gap_observation.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheLoads::bind(core::DagGraph & graph, model::HiCacheModelReplay & replay, uint64_t begin_us, uint64_t end_us) {
    if (state_) throw std::logic_error("Loads are already bound");
    state_ = &replay.state();
    graph_ = &graph;
    const patch::HiCacheSourceDagIndex source(graph);
    const auto ledger = patch::build_hicache_io_operation_ledger(source);
    const auto waits = observe_hicache_layer_waits(source);
    // Timing probes name the controller object; state facts name its radix
    // tree. Nonempty loads prove the association through node/request identity.
    // A nested write_backup/start_writing call proves the same object relation
    // without requiring this base to contain a nonempty load. Empty calls reuse
    // either explicit association, never a nearest-request or single-tree guess.
    auto tree_scopes = observe_write_controller_scopes(source);
    for (const auto & record : ledger.records) {
        if (record.kind != patch::HiCacheIoOperationKind::Load || record.request_id.empty() || record.cache_scope.empty()) continue;
        for (const auto & item : replay.facts()) {
            if (item.role != HiCacheFactRole::CacheLookupInput || item.fact.pid != record.pid || item.fact.request_id != record.request_id) continue;
            const auto [at, first] = tree_scopes.emplace(std::pair{ record.pid, record.cache_scope }, item.fact.cache_scope);
            if (!first && at->second != item.fact.cache_scope) throw std::runtime_error("Load controller maps to multiple cache trees");
        }
    }
    std::vector<core::TraceEvent> points;
    for (const auto & record : ledger.records) {
        if (record.kind != patch::HiCacheIoOperationKind::Load || record.source_start_us < begin_us || record.source_start_us > end_us) continue;
        patch::HiCacheRewriteDecision donor;
        donor.request_id = record.request_id;
        donor.source_fact_node_id = record.timing_fact_node_id;
        donor.source_readiness_topology_reused = record.source_readiness_topology_ready;
        donor.owned_duration_nodes = record.device_transfer_node_ids;
        const auto layers = patch::observe_hicache_layer_transfers(source, waits, donor);
        if (layers.empty() && !record.device_transfer_node_ids.empty()) throw std::runtime_error("Observed load has no complete layer layout");
        const auto lookup = std::ranges::find_if(replay.facts(), [&](const auto & item) {
            return item.role == HiCacheFactRole::CacheLookupInput && item.fact.pid == record.pid && item.fact.request_id == record.request_id;
        });
        if (!record.request_id.empty() && lookup == replay.facts().end())
            throw std::runtime_error("Load has no request lookup identity: pid=" + record.pid + " request=" + record.request_id
                                     + " fact=" + std::to_string(record.timing_fact_node_id));
        const auto tree = tree_scopes.find({ record.pid, record.cache_scope });
        if (tree == tree_scopes.end() || tree->second.empty()) throw std::runtime_error("Load controller has no observed cache-tree association");
        // start_loading drains a controller queue. An empty source call has no
        // request identity, but may still submit queued target work.
        HiCacheFact submission;
        submission.pid = record.pid;
        submission.tid = record.tid;
        submission.cache_scope = tree->second;
        submission.source_node_id = record.timing_fact_node_id;
        submission.request_id = record.request_id;
        if (lookup != replay.facts().end() && lookup->fact.cache_scope != submission.cache_scope)
            throw std::runtime_error("Load submission and request disagree on cache scope");
        loads_.push_back({ submission, record, layers, {} });
        core::TraceEvent point;
        point.name = "load_submission";
        point.pid = record.pid;
        point.tid = record.tid;
        point.ts = record.source_start_us;
        points.push_back(std::move(point));
    }
    std::set<std::pair<std::string, uint64_t>> batches;
    for (const auto & call : waits.calls) {
        if (call.batch_start_ns / 1'000 < begin_us || call.batch_start_ns / 1'000 > end_us) continue;
        if (!call.issue.empty() || !call.before) {
            auto detail = "Forward consumer needs a complete layer-wait observation: request=" + call.request_id + " phase=" + call.phase
                          + " layer=" + std::to_string(call.layer) + " position=" + std::to_string(call.position)
                          + " issue=" + (call.issue.empty() ? "missing_cpu_boundary" : call.issue);
            if (call.worker) detail += " stream_wait_binding=" + graph.event_for_node(*call.worker).arg("stream_wait_binding");
            throw std::runtime_error(detail);
        }
        const auto & anchor = graph.event_for_node(*call.before);
        if (!batches.emplace(anchor.pid, call.batch_start_ns).second) continue;
        if (call.phase != "EXTEND" && call.phase != "DECODE") throw std::runtime_error("Unsupported layer consumer forward mode");
        forwards_.push_back({ anchor.pid, call.request_id, call.batch_start_ns, call.phase == "EXTEND" });
        core::TraceEvent point;
        point.name = "layer_consumer_forward";
        point.pid = anchor.pid;
        point.tid = anchor.tid;
        point.ts = call.batch_start_ns / 1'000;
        points.push_back(std::move(point));
    }
    std::vector<core::TraceEvent> call_points;
    for (const auto & load : loads_)
        for (const auto at : { load.record.source_start_us, load.record.source_end_us }) {
            core::TraceEvent point;
            point.name = "load call boundary";
            point.pid = load.record.pid;
            point.tid = load.record.tid;
            point.ts = at;
            call_points.push_back(std::move(point));
        }
    const auto boundaries = bind_hicache_control_points(graph, call_points);
    const auto submissions = bind_hicache_control_points(graph, points);
    for (size_t i = 0; i < forwards_.size(); ++i) {
        const auto node = submissions.at(loads_.size() + i);
        if (!node) throw std::runtime_error("Forward consumer has no CPU boundary");
        forward_at_[*node].push_back(i);
    }
    core::DagMutationPlan source_costs{ .component = "hicache_load_transfer" };
    for (size_t i = 0; i < loads_.size(); ++i) {
        const auto & load = loads_[i];
        if (!submissions[i]) throw std::runtime_error("Load submission has no CPU boundary");
        if (!submit_at_.emplace(*submissions[i], i).second) throw std::runtime_error("Load submissions share an unresolved execution boundary");
        if (!submission_calibration_.empty()) continue;
        if (load.record.device_transfer_node_ids.empty()) continue;
        const auto first = *std::ranges::min_element(load.record.device_transfer_node_ids, {}, [&](size_t id) { return graph.event_for_node(id).ts; });
        const auto last = load.layers.back().source_record;
        if (!start_at_.emplace(first, i).second || !complete_at_.emplace(last, i).second)
            throw std::runtime_error("Source load batches share transfer or completion carriers");
        if (graph.node(last).duration) throw std::runtime_error("Load completion needs a zero-cost final Record boundary");
        for (const auto id : load.record.device_transfer_node_ids) source_costs.set_node_durations.push_back({ id, 0 });
    }
    (void)core::apply_dag_mutation_plan(graph, source_costs);

    prepare_generated_submissions(graph);
    prepare_insertions(graph, boundaries);
}

void HiCacheLoads::advance(size_t node, uint64_t time, simulation::FutureDag & future) {
    if (const auto at = submit_at_.find(node); at != submit_at_.end()) {
        auto & load = loads_[at->second];
        load.fact.ts = time;
        load.batch = state_->submit_loadbacks(load.fact);
        if (const auto site = insertions_.find(at->second); site != insertions_.end()) {
            const auto & insertion = site->second;
            if (!load.batch) {
                const auto empty = expand_host(insertion.empty, {}, future);
                future.depend(empty.host_return, insertion.continuation);
            }
            else {
                const Donor * donor = nullptr;
                uint64_t best = UINT64_MAX;
                const auto bytes = load.batch->io_schedule.effective_byte_count;
                for (const auto & candidate : donors_) {
                    // With independent operation costs, the target page count
                    // must generate submissions even when a base donor exists.
                    if (!submission_calibration_.empty()) break;
                    if (candidate.pid != load.fact.pid || candidate.tid != load.fact.tid) continue;
                    const auto size = candidate.plan.payload_bytes;
                    const auto distance = size > bytes ? size - bytes : bytes - size;
                    if (!donor || distance < best) {
                        donor = &candidate;
                        best = distance;
                    }
                }
                std::optional<Donor> generated;
                if (!donor) {
                    const auto program = generated_submissions_.find({ load.fact.pid, load.fact.tid });
                    if (program == generated_submissions_.end()) throw std::runtime_error("New load lacks independent submission costs or scheduler resources");
                    if (!page_bytes_ || bytes % page_bytes_) throw std::runtime_error("Target load is not whole KV pages");
                    const auto & p = program->second;
                    if (p.base_cost) ++base_cost_batches_;
                    else ++shared_cost_batches_;
                    auto built = generate_load_submission(*graph_, p.main, p.worker, p.compute, p.stream, bytes / page_bytes_, page_bytes_, p.layers, p.cost);
                    generated.emplace(Donor{
                        load.fact.pid,
                        load.fact.tid,
                        std::move(built.plan),
                        std::move(built.layer_records),
                        { graph_->node(p.compute).lane_id, graph_->node(p.stream).lane_id, graph_->node(p.compute).lane_id }
                    });
                    donor = &*generated;
                }
                HiCacheHostSequence sequence(insertion.positions, stream_insertions_, future);
                const auto expanded = sequence.append(donor->plan, load.batch->io_schedule.duration_us, donor->lanes);
                future.depend(expanded.host_return, insertion.continuation);
                for (const auto record : donor->records) load.layers.push_back({ load.layers.size(), 0, 0, expanded.nodes.at(record) });
                complete_at_.emplace(expanded.completion, at->second);
            }
        }
        const auto needed = load.fact.request_id.empty() ? std::vector<std::string>{} : state_->loadback_dependencies(load.fact);
        for (const auto & operation : needed)
            if (!load.batch || std::ranges::find(load.batch->operations, operation) == load.batch->operations.end())
                throw std::runtime_error("Request needs a load from another batch; connect its layer gates before continuing");
        std::vector<size_t> records;
        if (load.batch)
            for (const auto & layer : load.layers) records.push_back(layer.source_record);
        consumers_.submitted(load.fact.pid, std::move(records));
    }
    if (const auto at = start_at_.find(node); at != start_at_.end()) {
        auto & load = loads_[at->second];
        if (load.batch) {
            std::vector<uint64_t> bytes;
            std::vector<size_t> records;
            for (const auto & layer : load.layers) {
                bytes.push_back(layer.bytes);
                records.push_back(layer.source_record);
            }
            (void)generate_load_layer_transfers(future, node, load.batch->io_schedule.resource_lane, bytes, load.batch->io_schedule.duration_us, records);
        }
    }
    if (const auto at = complete_at_.find(node); at != complete_at_.end()) {
        auto & load = loads_[at->second];
        if (load.batch) {
            load.fact.ts = time;
            state_->complete_loadback_batch(load.fact, load.batch->id);
            ++completed_;
        }
    }
    if (const auto at = forward_at_.find(node); at != forward_at_.end())
        for (const auto index : at->second) {
            const auto & forward = forwards_.at(index);
            forward_records_.emplace(std::pair{ forward.pid, forward.start_ns }, consumers_.forward(forward.pid, forward.request, forward.extend));
        }
}

} // namespace markov::trace_graph::modules::hicache::runtime
