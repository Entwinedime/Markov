#include "markov/trace_graph/adapters/frameworks/sglang/source_calls.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/source_dag/index.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace markov::trace_graph::adapters::sglang {

std::optional<structure::hicache::CollectiveTaskKind> collective_task_kind(std::string_view name) {
    using Kind = structure::hicache::CollectiveTaskKind;
    if (name == "Enqueue@record_event") return Kind::Record;
    if (name == "Enqueue@wait_event") return Kind::Wait;
    if (name == "Enqueue@HcclAllreduce") return Kind::Reduce;
    if (name == "Enqueue@HcclAllGather") return Kind::Gather;
    if (name == "Enqueue@destroy_event") return Kind::Destroy;
    return std::nullopt;
}

std::optional<std::vector<CollectiveCallerInterval>> observe_collective_caller_intervals(const source_dag::SourceDagIndex & source,
                                                                                         const simulation::detail::CpuTaskQueues & queues,
                                                                                         const DeviceCollectiveCall & call,
                                                                                         std::span<const source_dag::QueuedCallTask> tasks) {
    if (!call.nonblocking_group) return std::nullopt;
    const auto steps = structure::hicache::ordinary_collective_caller_steps(call.transfer);
    if (tasks.size() + 1 != steps.size()) return std::nullopt;
    const auto & graph = source.graph();
    for (const auto node : call.region.caller.owned_node_ids) {
        const auto & event = graph.event_for_node(node);
        if (core::synchronizes_recorded_event(event) || core::synchronizes_device_frontier(event)) return std::nullopt;
        for (const auto id : source.incoming_edge_ids(node)) {
            const auto & edge = graph.edge(id);
            if (edge.kind == core::DagEdgeKind::Sync && !graph.node(edge.src).is_cpu) return std::nullopt;
        }
    }
    const auto & envelope = graph.runtime_observations().at(call.region.observation);
    const auto end = core::checked_add_u64(envelope.ts, envelope.dur, "Collective caller end overflow");
    auto cursor = envelope.ts;
    std::vector<CollectiveCallerInterval> intervals;
    for (size_t i = 0; i < tasks.size(); ++i) {
        const auto submission = queues.tasks.at(tasks[i].task).submission;
        const auto & event = graph.event_for_node(submission);
        const auto boundary = core::checked_add_u64(event.ts, event.dur, "Collective enqueue end overflow");
        if (collective_task_kind(event.name) != steps[i].task || event.ts < cursor || boundary > end) return std::nullopt;

        intervals.push_back({ i, cursor, boundary, submission });
        cursor = boundary;
    }
    // Repeated record_event labels alone cannot identify the future and
    // work-completion events. Corroborate both stream handoffs by their
    // actual producer/consumer identities before assigning caller roles.
    const auto joined = [&](size_t record_step, size_t wait_step) {
        if (tasks[record_step].devices.size() != 1 || tasks[wait_step].devices.size() != 1) return false;
        const auto record = *tasks[record_step].devices.begin(), wait = *tasks[wait_step].devices.begin();
        const auto & wait_name = graph.event_for_node(wait).name;
        const bool event_wait = wait_name == "EVENT_WAIT" || (wait_name == "logical_event_wait" && graph.node(wait).kind == core::DagNodeKind::Synthetic);
        if (graph.event_for_node(record).name != "EVENT_RECORD" || !event_wait) return false;
        return std::ranges::any_of(source.incoming_edge_ids(wait), [&](size_t id) {
            const auto & edge = graph.edge(id);
            return edge.kind == core::DagEdgeKind::Sync && edge.src == record;
        });
    };
    if (!joined(0, 1) || !joined(4, 5)) return std::nullopt;

    intervals.push_back({ steps.size() - 1, cursor, end, std::nullopt });
    return intervals;
}

namespace {

using GroupIdentity = std::tuple<std::string, std::string, uint64_t>;

// Only work shared by all ranks belongs in the peer comparison. Rank-local
// observation coordinates and queue ids never become cross-trace identities.
struct CollectiveGeometry {
    std::vector<int64_t> members;
    std::string dtype, role, callsite;
    uint64_t layer_index, rows, width, element_bytes;

    bool operator==(const CollectiveGeometry &) const = default;
};

struct CollectiveDescriptor {
    CollectiveGeometry work;
    int64_t global_rank;
};

struct CollectiveCandidate {
    source_dag::SubmittedCallRegion region;
    std::optional<PhaseCallPosition> position;
    std::optional<CollectiveDescriptor> descriptor;
    bool shared_device = false;
};

std::optional<CollectiveDescriptor> ordinary_descriptor(const nlohmann::json & fields, bool gather) {
    // Probe metadata is an external boundary. Admit the supported packed
    // Qwen3 TP operation once; internal peer matching then uses typed work.
    for (const auto * key : { "group_members",
                              "group_rank",
                              "global_rank",
                              "world_size",
                              "input_shape",
                              "input_stride",
                              "element_bytes",
                              "input_dtype",
                              "input_device_type",
                              "model_layer_implementation",
                              "model_layer_index",
                              "dense_role",
                              "callsite",
                              "operation",
                              "status" })
        if (!fields.contains(key) || fields.at(key).is_null()) return std::nullopt;

    const auto & members = fields.at("group_members");
    if (!members.is_array() || !fields.at("group_rank").is_number_integer() || !fields.at("global_rank").is_number_integer()
        || !fields.at("world_size").is_number_integer())
        return std::nullopt;

    const auto rank = fields.at("group_rank").get<int64_t>(), world = fields.at("world_size").get<int64_t>();
    std::set<int64_t> unique_members;
    for (const auto & member : members) {
        if (!member.is_number_integer() || member.get<int64_t>() < 0) return std::nullopt;
        unique_members.insert(member.get<int64_t>());
    }
    if (world <= 0 || members.size() != static_cast<size_t>(world) || unique_members.size() != members.size() || rank < 0 || rank >= world
        || members.at(static_cast<size_t>(rank)) != fields.at("global_rank"))
        return std::nullopt;

    const auto &shape = fields.at("input_shape"), &stride = fields.at("input_stride");
    if (!shape.is_array() || shape.size() != 2 || !stride.is_array() || !fields.at("element_bytes").is_number_integer()
        || fields.at("element_bytes").get<int64_t>() <= 0
        || !std::ranges::all_of(shape, [](const auto & value) { return value.is_number_integer() && value.template get<int64_t>() > 0; })
        || !std::ranges::all_of(stride, [](const auto & value) { return value.is_number_integer() && value.template get<int64_t>() >= 0; })
        || stride != nlohmann::json::array({ shape.at(1), 1 }) || fields.at("input_device_type") != "npu"
        || fields.at("model_layer_implementation") != "sglang.srt.models.qwen3.Qwen3DecoderLayer" || !fields.at("model_layer_index").is_number_integer()
        || fields.at("model_layer_index").get<int64_t>() < 0 || fields.at("operation") != (gather ? "gather" : "sum") || fields.at("status") != "returned")
        return std::nullopt;

    CollectiveGeometry work{ members.get<std::vector<int64_t>>(),
                             fields.at("input_dtype").get<std::string>(),
                             fields.at("dense_role").get<std::string>(),
                             fields.at("callsite").get<std::string>(),
                             fields.at("model_layer_index").get<uint64_t>(),
                             shape.at(0).get<uint64_t>(),
                             shape.at(1).get<uint64_t>(),
                             fields.at("element_bytes").get<uint64_t>() };
    const auto row_bytes = core::checked_multiply_u64(work.element_bytes, work.rows, "collective tensor byte count overflow");
    (void)core::checked_multiply_u64(row_bytes, work.width, "collective tensor byte count overflow");
    if (gather) {
        for (const auto * key : { "gather_dim", "output_shape", "output_stride", "output_dtype", "last_token_projection" })
            if (!fields.contains(key) || fields.at(key).is_null()) return std::nullopt;

        const auto width = core::checked_multiply_u64(work.width, work.members.size(), "Gather output width overflow");
        if (work.rows != 1 || fields.at("gather_dim") != 1 || fields.at("last_token_projection") != true || work.role != "logits_projection"
            || work.callsite != "_get_logits" || fields.at("output_shape") != nlohmann::json::array({ 1, width })
            || fields.at("output_stride") != nlohmann::json::array({ width, 1 }) || fields.at("output_dtype") != fields.at("input_dtype"))
            return std::nullopt;
    }

    return CollectiveDescriptor{ std::move(work), fields.at("global_rank").get<int64_t>() };
}

} // namespace

DeviceCollectiveAudit observe_device_collectives(const source_dag::SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                                 const modules::hicache::HiCachePhaseObservationAudit & phases, const PhaseCallPositions & positions) {
    const auto & graph = source.graph();
    std::vector<CollectiveCandidate> candidates;
    std::map<GroupIdentity, std::vector<size_t>> groups;
    std::map<size_t, size_t> device_owners;
    for (size_t i = 0; i < graph.runtime_observations().size(); ++i) {
        const auto & event = graph.runtime_observations()[i];
        if (event.source_channel != core::TraceSourceChannel::PythonProbe || event.name != "runtime.phase.device_collective"
            || event.arg("formal_window_context") == "causal_head")
            continue;

        const auto implementation = event.arg("implementation");
        const bool gather = implementation == "sglang.NpuCommunicator.all_gather";
        if (!gather && implementation != "sglang.NpuCommunicator.all_reduce") continue;

        auto region = source_dag::observe_submitted_call_region(source, queues, i);
        const bool owned = region.caller.status == "ready" && !region.caller.has_node_overlap;
        const auto position = owned ? call_phase_position(region, positions) : std::nullopt;
        const auto fields = nlohmann::json::parse(event.args_json_view());
        const auto row = candidates.size();
        candidates.push_back({ std::move(region), position, ordinary_descriptor(fields, gather) });
        if (owned) {
            for (const auto node : candidates.back().region.submitted_nodes) {
                if (graph.node(node).is_cpu) continue;
                const auto [owner, added] = device_owners.emplace(node, row);
                if (!added) {
                    candidates[owner->second].shared_device = true;
                    candidates.back().shared_device = true;
                }
            }
        }

        // Even an invalid descriptor stays in its nominated group. Omitting it
        // could turn incomplete or duplicate-rank evidence into valid peers.
        const auto group = fields.find("group"), sequence = fields.find(gather ? "observed_all_gather_index" : "observed_all_reduce_index");
        if (group != fields.end() && group->is_string() && sequence != fields.end() && sequence->is_number_integer() && sequence->get<int64_t>() >= 0)
            groups[{ group->get<std::string>(), implementation, sequence->get<uint64_t>() }].push_back(row);
    }

    DeviceCollectiveAudit audit;
    for (const auto & [identity, rows] : groups) {
        const auto & first = candidates[rows.front()];
        if (!first.descriptor || !first.position) continue;
        const auto & work = first.descriptor->work;
        const auto & phase = phases.observations.at(first.position->phase_index);
        std::map<int64_t, size_t> participants;
        bool complete = true;
        for (const auto row : rows) {
            const auto & candidate = candidates[row];
            if (!candidate.descriptor || !candidate.position || candidate.shared_device || candidate.descriptor->work != work
                || candidate.position->decode_iteration != first.position->decode_iteration
                || phases.observations.at(candidate.position->phase_index).request_ids != phase.request_ids
                || !std::ranges::any_of(candidate.region.submitted_nodes, [&](size_t node) { return !graph.node(node).is_cpu; })
                || !participants.emplace(candidate.descriptor->global_rank, row).second) {
                complete = false;
                break;
            }
        }
        if (!complete || participants.size() != work.members.size()) continue;

        std::vector<size_t> peers;
        for (const auto rank : work.members) {
            const auto peer = participants.find(rank);
            if (peer == participants.end()) {
                complete = false;
                break;
            }
            peers.push_back(candidates[peer->second].region.observation);
        }
        if (!complete) continue;

        const bool gather = std::get<1>(identity) == "sglang.NpuCommunicator.all_gather";
        for (const auto row : rows) {
            auto & candidate = candidates[row];
            const auto position = *candidate.position;
            const auto & owned_phase = phases.observations.at(position.phase_index);
            const auto & phase_nodes = position.decode_iteration ? owned_phase.decode.collective_node_ids : owned_phase.prefill.collective_node_ids;
            const std::set<size_t> submitted(candidate.region.submitted_nodes.begin(), candidate.region.submitted_nodes.end());
            std::vector<size_t> communication_nodes;
            for (const auto node : phase_nodes)
                if (submitted.contains(node)) communication_nodes.push_back(node);

            audit.calls.push_back({ std::move(candidate.region),
                                    position,
                                    std::get<0>(identity),
                                    work.dtype,
                                    work.role,
                                    work.callsite,
                                    std::get<2>(identity),
                                    work.layer_index,
                                    work.rows,
                                    work.width,
                                    work.element_bytes,
                                    candidate.descriptor->global_rank,
                                    work.members,
                                    peers,
                                    std::move(communication_nodes),
                                    false,
                                    gather ? structure::hicache::CollectiveTaskKind::Gather : structure::hicache::CollectiveTaskKind::Reduce });
        }
    }

    std::set<std::pair<std::string, int64_t>> nonblocking_groups;
    for (const auto & call : audit.calls) {
        const auto & envelope = graph.runtime_observations().at(call.region.observation);
        const auto returned = core::checked_add_u64(envelope.ts, envelope.dur, "Collective return overflow");
        for (const auto node : call.communication_nodes) {
            const auto & device = graph.event_for_node(node);
            // Raw completion, not normalized HCCL duration or a peer wait.
            // Strict tick separation avoids inferring order from equal ticks.
            const auto completed = core::checked_add_u64(device.ts, device.dur, "Collective completion overflow");
            if (returned < completed) nonblocking_groups.emplace(call.group, call.global_rank);
        }
    }
    for (auto & call : audit.calls) call.nonblocking_group = nonblocking_groups.contains({ call.group, call.global_rank });

    std::set<size_t> attributed;
    for (const auto & call : audit.calls) attributed.insert(call.region.submitted_nodes.begin(), call.region.submitted_nodes.end());
    for (const auto & phase : phases.observations)
        for (const auto * nodes : { &phase.prefill.collective_node_ids, &phase.decode.collective_node_ids })
            for (const auto node : *nodes)
                if (!attributed.contains(node)) ++audit.issues["phase_collective_without_attributed_call"];

    return audit;
}

} // namespace markov::trace_graph::adapters::sglang
