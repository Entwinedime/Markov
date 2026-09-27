#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/control_calibration.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheWriteCalls::load_release_calibration(const core::DagGraph & graph, const simulation::detail::CpuTaskQueues & queues) {
    if (release_host_calibration_.empty()) return;
    std::ifstream file(release_host_calibration_);
    if (!file) throw std::runtime_error("Cannot read independent release host calibration");
    const auto input = nlohmann::json::parse(file);
    if (input.at("scope") != "ordinary release resource evidence" || input.at("source_manifest").get<std::string>().empty()
        || input.value("cost_basis", "") != "paired_cpu_service" || input.value("cpu_service_file", "").empty())
        throw std::runtime_error("Shared ordinary release requires capture provenance and paired CPU correction");
    using Kind = HiCacheEvictionRegion::Kind;
    std::set<std::pair<std::string, std::string>> seen;
    std::vector<EvictionControl> imported;
    for (const auto & base : eviction_controls_) {
        if ((base.region.kind != Kind::ReleaseBackup && base.region.kind != Kind::ReleaseRegular) || seen.contains({ base.pid, base.tid })) continue;
        if (base.expansion.streams.size() != 1) continue;
        const auto main_lane = base.expansion.nodes.at(base.expansion.host_return).work.lane_key;
        std::optional<size_t> main;
        for (const auto & [original, local] : base.expansion.source_nodes)
            if (graph.node(original).is_cpu && graph.node_lane_key(original) == main_lane) {
                main = original;
                break;
            }
        if (!main) continue;
        // Both branches call allocator.free. Reuse only its resources; the
        // ordinary-release CPU work and dependencies come from calibration.
        const auto roles = host_resource_roles(graph, base.expansion, *main, base.expansion.streams.front().source_node);
        const auto rank = graph.node(*main).gpu_id;
        const auto covered = [&](const HiCacheEvictionRegion & region) {
            return std::ranges::any_of(eviction_controls_, [&](const auto & donor) {
                return donor.pid == base.pid && donor.tid == base.tid && donor.region.kind == region.kind && donor.region.control_phase == region.control_phase
                       && donor.region.preceding_kind == region.preceding_kind;
            });
        };
        for (const auto & sample : input.at("rows")) {
            if (sample.at("status") != "ready" || sample.at("rank").get<int>() != rank) continue;
            HiCacheEvictionRegion region{};
            region.kind = Kind::ReleaseRegular;
            if (covered(region)) continue;
            const auto tokens = sample.at("released_tokens").get<uint64_t>();
            if (!tokens) throw std::runtime_error("Independent release template has no released tokens");
            auto expansion = import_host_template(sample.at("template"), graph, queues, roles);
            if (!expansion.event_waits.empty()) throw std::runtime_error("Ordinary release cannot wait for backup completion");
            region.released_tokens = tokens;
            imported.push_back({ base.pid, base.tid, region, std::move(expansion), {}, true });
        }
        // Ordinary eviction also has work between consecutive releases. It
        // does not require a write-back submission or completion template.
        if (input.contains("eviction_controls"))
            for (const auto & sample : input.at("eviction_controls")) {
                if (sample.at("status") != "ready" || sample.at("rank").get<int>() != rank || sample.at("kind").get<int>() != static_cast<int>(Kind::Control)
                    || !sample.contains("preceding_operation")
                    || sample.at("preceding_operation").at("kind").get<int>() != static_cast<int>(Kind::ReleaseRegular))
                    continue;
                HiCacheEvictionRegion region{};
                region.kind = Kind::Control;
                region.control_phase = static_cast<HiCacheEvictionRegion::ControlPhase>(sample.at("phase").get<int>());
                region.preceding_kind = Kind::ReleaseRegular;
                if (covered(region)) continue;
                auto expansion = import_host_template(sample.at("template"), graph, queues, roles);
                imported.push_back({ base.pid, base.tid, region, std::move(expansion), {}, true });
            }
        seen.emplace(base.pid, base.tid);
    }
    for (auto & sample : imported) eviction_controls_.push_back(std::move(sample));
}

void HiCacheWriteCalls::load_write_calibration(core::DagGraph & graph, const simulation::detail::CpuTaskQueues & queues) {
    if (write_host_calibration_.empty()) return;
    std::ifstream file(write_host_calibration_);
    if (!file) throw std::runtime_error("Cannot read independent write host calibration");
    const auto input = nlohmann::json::parse(file);
    if (input.at("role") != "fixed_calibration" || input.at("source_manifest").get<std::string>().empty())
        throw std::runtime_error("Write host templates require explicit independent calibration provenance");
    std::set<std::pair<std::string, std::string>> seen;
    std::vector<WriteTemplate> imported;
    std::vector<EvictionControl> imported_controls;
    std::set<std::pair<std::string, std::string>> controls_ready;
    const auto import = [&](const std::string & pid, const std::string & tid, const std::map<std::string, size_t> & roles) {
        const auto rank = graph.node(roles.at("main")).gpu_id;
        for (const auto & sample : input.at("rows")) {
            if (sample.at("status") != "ready" || sample.at("rank").get<int>() != rank || sample.at("write_back").get<bool>() != write_back_) continue;
            auto sample_roles = roles;
            if (sample.contains("worker_submissions")) bind_calibration_workers(sample.at("worker_submissions"), graph, queues, sample_roles);
            auto expansion = import_write_template(sample.at("template"), graph, queues, sample_roles);
            auto lanes = expansion.resource_lanes(graph);
            imported.push_back({ sample.at("write_back").get<bool>(), roles.at("compute_ready"), std::move(expansion), pid, tid, std::move(lanes), true });
        }
    };
    for (const auto & [id, base] : templates_) {
        if (!seen.emplace(base.pid, base.tid).second) continue;
        import(base.pid, base.tid, write_resource_roles(graph, base.expansion, base.compute_record));
        if (base.write_back == std::optional<bool>{ write_back_ }) controls_ready.emplace(base.pid, base.tid);
    }
    for (const auto & base : eviction_controls_) {
        if ((seen.contains({ base.pid, base.tid }) && (!write_back_ || controls_ready.contains({ base.pid, base.tid })))
            || (base.region.kind != HiCacheEvictionRegion::Kind::ReleaseRegular && base.region.kind != HiCacheEvictionRegion::Kind::ReleaseBackup)
            || base.expansion.streams.size() != 1)
            continue;
        const auto & lane = base.expansion.nodes.at(base.expansion.host_return).work.lane_key;
        std::optional<size_t> main;
        for (const auto & [original, local] : base.expansion.source_nodes)
            if (graph.node(original).is_cpu && graph.node_lane_key(original) == lane) {
                main = original;
                break;
            }
        if (!main) continue;
        const auto compute = base.expansion.streams.front().source_node;
        // Both release branches call allocator.free on compute; validate every device node
        // against that witness before introducing the separate write stream.
        const auto control_roles = host_resource_roles(graph, base.expansion, *main, compute);
        const auto rank = graph.node(*main).gpu_id;
        const bool covered = std::ranges::any_of(input.at("rows"), [&](const auto & sample) {
            return sample.at("status") == "ready" && sample.at("rank") == rank && sample.at("write_back") == write_back_;
        });
        if (!covered) continue;
        if (!seen.contains({ base.pid, base.tid })) {
            const auto resource = graph.add_synthetic_node({
                .name = "unused HiCache write stream",
                .is_cpu = false,
                .lane_key = "hicache.write:" + std::to_string(graph.node_count()),
                .duration = 0,
                .observed_point = core::DagObservedPoint{ base.pid, base.tid, 0, rank }
            });
            import(base.pid,
                   base.tid,
                   {
                       {          "main",    *main },
                       { "compute_ready",  compute },
                       {       "payload", resource }
            });
            graph.mutable_node(resource).active = false; // Resource identity, not executable work.
        }
        if (write_back_ && input.contains("eviction_controls")) {
            for (const auto & sample : input.at("eviction_controls")) {
                if (sample.at("status") != "ready" || sample.at("rank") != rank) continue;
                auto roles = control_roles;
                nlohmann::json submissions = nlohmann::json::object();
                for (const auto & node : sample.at("template").at("nodes")) {
                    const auto role = node.at("resource").get<std::string>();
                    if (!role.starts_with("worker|")) continue;
                    // host_resource_roles encodes original submission names,
                    // unlike write roles containing generated work labels.
                    auto names = nlohmann::json::array();
                    for (size_t at = 7; at < role.size();) {
                        const auto end = role.find('|', at);
                        names.push_back(role.substr(at, end == std::string::npos ? end : end - at));
                        if (end == std::string::npos) break;
                        at = end + 1;
                    }
                    submissions[role] = std::move(names);
                }
                bind_calibration_workers(submissions, graph, queues, roles);
                auto expansion = import_host_template(sample.at("template"), graph, queues, roles);
                HiCacheEvictionRegion region{};
                region.kind = static_cast<HiCacheEvictionRegion::Kind>(sample.at("kind").get<int>());
                region.control_phase = static_cast<HiCacheEvictionRegion::ControlPhase>(sample.at("phase").get<int>());
                region.released_tokens = sample.at("released_tokens").get<uint64_t>();
                if (sample.contains("preceding_operation"))
                    region.preceding_kind = static_cast<HiCacheEvictionRegion::Kind>(sample.at("preceding_operation").at("kind").get<int>());
                imported_controls.push_back({ base.pid, base.tid, region, std::move(expansion), {}, true });
            }
        }
        seen.emplace(base.pid, base.tid);
        controls_ready.emplace(base.pid, base.tid);
    }
    for (auto & item : imported_controls) eviction_controls_.push_back(std::move(item));
    for (auto & sample : imported) templates_.emplace(graph.node_count() + templates_.size(), std::move(sample));
}

void HiCacheWriteCalls::bind_lifecycle_writes(core::DagGraph & graph) {
    std::vector<core::TraceEvent> points;
    std::vector<size_t> owners;
    for (const auto & insert : lifecycle_inserts_) {
        // Existing source submissions are consumed at their own call sites.
        if (source_owners_.contains(insert.owner)) continue;
        const auto append = [&](core::TraceEvent point) {
            point.dur = point.dur_submicro_ns = 0;
            points.push_back(std::move(point));
            owners.push_back(insert.owner);
        };
        for (const auto & step : lifecycle_ready_steps(insert)) append(step.ready);
        auto end = insert.envelope;
        const auto ns = end.ts_submicro_ns + end.dur_submicro_ns;
        end.ts += end.dur + ns / 1'000;
        end.ts_submicro_ns = ns % 1'000;
        append(std::move(end));
    }
    const auto nodes = bind_hicache_control_points(graph, points);
    const patch::HiCacheSourceDagIndex source(graph);
    for (size_t i = 0; i < points.size(); ++i) {
        if (!nodes[i]) throw std::runtime_error("Lifecycle write lacks an exact ready boundary");
        Call call{ .owner = owners[i], .pid = points[i].pid, .tid = points[i].tid, .at_us = points[i].ts, .entry_node = *nodes[i] };
        for (const auto edge_id : source.outgoing_edge_ids(*nodes[i])) {
            const auto & edge = graph.edge(edge_id);
            if (edge.active && graph.node(edge.dst).active) call.successors.push_back(edge.dst);
        }
        for (const auto & [id, donor] : templates_) {
            if (donor.pid != call.pid || donor.tid != call.tid || donor.write_back != std::optional<bool>{ write_back_ }) continue;
            const auto locate = [&](size_t sample) {
                const auto lane = graph.node(sample).lane_id;
                if (!call.positions.contains(lane)) call.positions.emplace(lane, observe_write_stream_position(source, sample, call.pid, call.tid, call.at_us));
            };
            for (const auto node : donor.expansion.resource_nodes()) locate(node);
        }
        lifecycle_write_owners_.insert(call.owner);
        lifecycle_writes_at_[*nodes[i]].push_back(std::move(call));
    }
}

void HiCacheWriteCalls::submit_lifecycle_writes(size_t node, uint64_t time, simulation::FutureDag & future) {
    const auto at = lifecycle_writes_at_.find(node);
    if (at == lifecycle_writes_at_.end()) return;
    for (const auto & call : at->second) {
        auto fact = replay_.fact(call.owner).fact;
        fact.ts = time;
        fact.execution_anchor_node_id = node;
        HiCacheHostSequence sequence(call.positions, stream_insertions_, future);
        for (const auto & pending : replay_.state().pending_device_writes(fact)) {
            const auto key = std::pair{ pending.header.cache_scope, pending.header.operation_id };
            if (pending.header.source_node_id != call.owner || completions_.contains(key)) continue;
            const auto * donor = &select_write_template(call.pid, call.tid, pending.schedule.effective_byte_count);
            const auto projected = resize_write_pages(donor->expansion, pending.schedule.effective_byte_count, page_bytes_);
            const auto expanded = sequence.append(projected, pending.schedule.duration_us, donor->position_lanes);
            for (const auto successor : call.successors) future.depend(expanded.host_return, successor);
            const auto done = writes_.submit_expanded(fact, pending, expanded.write_start, expanded.completion, future);
            completions_.emplace(key, done);
            ++expanded_;
        }
    }
}

} // namespace markov::trace_graph::modules::hicache::runtime
