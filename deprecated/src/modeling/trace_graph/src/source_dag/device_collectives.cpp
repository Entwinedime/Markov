#include "markov/trace_graph/source_dag/device_collectives.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/source_dag/call_regions.hpp"
#include "markov/trace_graph/source_dag/observations.hpp"

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

namespace markov::trace_graph::source_dag {

std::optional<structure::hicache::CollectiveTaskKind> collective_task_kind(std::string_view name) {
    using Kind = structure::hicache::CollectiveTaskKind;
    if (name == "Enqueue@record_event") return Kind::Record;
    if (name == "Enqueue@wait_event") return Kind::Wait;
    if (name == "Enqueue@HcclAllreduce") return Kind::Reduce;
    if (name == "Enqueue@HcclAllGather") return Kind::Gather;
    if (name == "Enqueue@destroy_event") return Kind::Destroy;
    return std::nullopt;
}

std::optional<std::vector<CollectiveCallerInterval>> observe_collective_caller_intervals(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                                                         const simulation::detail::CpuTaskQueues & queues,
                                                                                         const DeviceCollectiveCall & call,
                                                                                         std::span<const QueuedCallTask> tasks) {
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

// Group-local counters only nominate peers. Every rank must independently
// corroborate the request, round, layer, callsite and tensor operation.
nlohmann::json collective_peer_groups(const nlohmann::json & calls, const modules::hicache::patch::HiCacheSourceDagIndex & source) {
    std::map<std::tuple<std::string, std::string, uint64_t>, std::vector<size_t>> candidates;
    for (size_t row = 0; row < calls.size(); ++row) {
        const auto & descriptor = calls[row].at("descriptor");
        if (!descriptor.at("group").is_string() || !descriptor.at("observed_collective_index").is_number_integer()
            || descriptor.at("observed_collective_index").get<int64_t>() < 0)
            continue;
        candidates[{ descriptor.at("group").get<std::string>(),
                     descriptor.at("implementation").get<std::string>(),
                     descriptor.at("observed_collective_index").get<uint64_t>() }]
            .push_back(row);
    }

    auto groups = nlohmann::json::array();
    for (const auto & [identity, rows] : candidates) {
        const auto & first = calls.at(rows.front());
        const auto & descriptor = first.at("descriptor");
        std::set<int64_t> ranks;
        bool consistent = true;
        for (const auto row : rows) {
            const auto & call = calls.at(row);
            const auto & peer = call.at("descriptor");
            if (!call.at("group_consistent").get<bool>() || call.at("phase_position").is_null() || call.at("logical_tensor_bytes").is_null()
                || call.at("device_nodes").empty() || !call.at("shared_device_nodes").empty() || peer.at("status") != "returned") {
                consistent = false;
                continue;
            }
            if (!ranks.insert(peer.at("global_rank").get<int64_t>()).second) consistent = false;
            for (const auto * key : { "group_members",
                                      "world_size",
                                      "implementation",
                                      "operation",
                                      "input_shape",
                                      "input_stride",
                                      "input_dtype",
                                      "input_device_type",
                                      "element_bytes",
                                      "model_layer_implementation",
                                      "model_layer_index",
                                      "dense_role",
                                      "callsite" }) {
                if (peer.at(key).is_null() || peer.at(key) != descriptor.at(key)) consistent = false;
            }
            if (descriptor.at("implementation") == "sglang.NpuCommunicator.all_gather")
                for (const auto * key : { "gather_dim", "output_shape", "output_stride", "output_dtype", "last_token_projection" })
                    if (peer.at(key).is_null() || peer.at(key) != descriptor.at(key)) consistent = false;
            if (first.at("phase_position").is_null()) {
                consistent = false;
                continue;
            }
            for (const auto * key : { "request_ids", "phase", "decode_iteration" }) {
                if (call.at("phase_position").at(key) != first.at("phase_position").at(key)) consistent = false;
            }
        }

        // The validated roster contains unique global ranks in group order.
        // Equality, not merely participant count, prevents a partial capture
        // or an extra rank from being accepted as a complete collective.
        std::set<int64_t> expected;
        if (first.at("group_consistent").get<bool>()) {
            for (const auto & rank : descriptor.at("group_members")) expected.insert(rank.get<int64_t>());
        }
        consistent &= !expected.empty() && ranks == expected;
        auto completion_links = nlohmann::json::array();
        size_t unavailable_boundaries = 0;
        if (consistent) {
            std::map<size_t, DeviceChain> chains;
            for (const auto row : rows) {
                std::set<size_t> devices;
                const auto & call = calls.at(row);
                const auto transfer = descriptor.at("implementation") == "sglang.NpuCommunicator.all_gather" ? structure::hicache::CollectiveTaskKind::Gather
                                                                                                             : structure::hicache::CollectiveTaskKind::Reduce;
                // Peer completion belongs to the actual communication work.
                // The whole caller also submits event record/wait tasks on
                // multiple streams; its last event is not an HCCL completion.
                if (call.at("queued_task_ownership_complete").get<bool>()) {
                    for (const auto & task : call.at("queued_tasks")) {
                        if (collective_task_kind(task.at("implementation").get<std::string>()) != transfer) continue;
                        for (const auto & node : task.at("device_nodes")) devices.insert(node.get<size_t>());
                    }
                }
                if (const auto chain = observe_serial_device_chain(source, devices)) chains.emplace(row, *chain);
                else ++unavailable_boundaries;
            }
            for (const auto & [row, chain] : chains) {
                const auto successor = source.graph().node(chain.last).hccl_successor_node_id;
                if (successor == core::DagNode::kNoNode) {
                    ++unavailable_boundaries;
                    continue;
                }
                for (const auto & [peer_row, peer_chain] : chains) {
                    if (peer_row == row) continue;
                    const auto incoming = source.incoming_edge_ids(successor);
                    const bool connected = std::ranges::any_of(incoming, [&](size_t edge_id) {
                        const auto & edge = source.graph().edge(edge_id);
                        return edge.active && edge.src == peer_chain.last && edge.kind == core::DagEdgeKind::HCCL;
                    });
                    completion_links.push_back({
                        {      "peer_call_row",        peer_row },
                        {  "consumer_call_row",             row },
                        {    "peer_completion", peer_chain.last },
                        {           "consumer",       successor },
                        { "explicit_hccl_edge",       connected }
                    });
                }
            }
        }
        groups.push_back({
            {                             "group",                                                         std::get<0>(identity) },
            {                    "implementation",                                                         std::get<1>(identity) },
            {         "observed_collective_index",                                                         std::get<2>(identity) },
            {                         "call_rows",                                                                          rows },
            {         "descriptor_correspondence",                                    consistent ? "corroborated" : "unresolved" },
            { "unavailable_completion_boundaries", consistent ? nlohmann::json(unavailable_boundaries) : nlohmann::json(nullptr) },
            {      "source_peer_completion_links",                                                   std::move(completion_links) }
        });
    }
    return groups;
}

struct CollectedDeviceCollectives {
    DeviceCollectiveAudit audit;
    nlohmann::json report;
};

CollectedDeviceCollectives collect_device_collectives(const core::DagGraph & graph, const modules::hicache::HiCachePhaseObservationAudit & phases) {
    const modules::hicache::patch::HiCacheSourceDagIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    const auto positions = phase_call_positions(phases);
    auto calls = nlohmann::json::array();
    auto unmodeled_calls = nlohmann::json::array();
    std::vector<SubmittedCallRegion> regions;
    std::vector<std::optional<PhaseCallPosition>> owners;
    std::map<size_t, std::vector<size_t>> device_owners;
    std::map<std::string, size_t> issues;

    for (size_t i = 0; i < graph.runtime_observations().size(); ++i) {
        const auto & event = graph.runtime_observations()[i];
        if (event.source_channel != core::TraceSourceChannel::PythonProbe || event.name != "runtime.phase.device_collective") continue;

        const auto fields = nlohmann::json::parse(event.args_json_view());
        const bool gather = event.arg("implementation") == "sglang.NpuCommunicator.all_gather";
        if (!gather && event.arg("implementation") != "sglang.NpuCommunicator.all_reduce") {
            // Unknown operations retain evidence without being assigned the
            // queued protocol of an ordinary reduction or gather.
            unmodeled_calls.push_back({
                { "observation",      i },
                {  "descriptor", fields }
            });
            ++issues["collective_implementation_not_lowered"];
            continue;
        }
        auto descriptor = nlohmann::json::object();
        for (const auto * key : { "implementation",
                                  "operation",
                                  "status",
                                  "group",
                                  "group_members",
                                  "global_rank",
                                  "group_rank",
                                  "world_size",
                                  "input_shape",
                                  "input_stride",
                                  "input_dtype",
                                  "input_device_type",
                                  "element_bytes",
                                  "model_layer_implementation",
                                  "model_layer_index",
                                  "dense_role",
                                  "callsite" }) {
            descriptor[key] = fields.contains(key) ? fields.at(key) : nlohmann::json(nullptr);
        }
        const auto sequence = gather ? "observed_all_gather_index" : "observed_all_reduce_index";
        descriptor["observed_collective_index"] = fields.contains(sequence) ? fields.at(sequence) : nlohmann::json(nullptr);
        if (gather)
            for (const auto * key : { "gather_dim", "output_shape", "output_stride", "output_dtype", "last_token_projection" })
                descriptor[key] = fields.contains(key) ? fields.at(key) : nlohmann::json(nullptr);
        if (!descriptor["group"].is_string() || !descriptor["observed_collective_index"].is_number_integer()
            || descriptor["observed_collective_index"].get<int64_t>() < 0)
            ++issues["missing_group_sequence"];

        bool group_consistent = false;
        if (descriptor["group_members"].is_array() && descriptor["group_rank"].is_number_integer() && descriptor["global_rank"].is_number_integer()
            && descriptor["world_size"].is_number_integer()) {
            const auto & members = descriptor["group_members"];
            const auto rank = descriptor["group_rank"].get<int64_t>();
            const auto world = descriptor["world_size"].get<int64_t>();
            std::set<int64_t> unique_members;
            bool valid_members = true;
            for (const auto & member : members) {
                if (!member.is_number_integer() || member.get<int64_t>() < 0) {
                    valid_members = false;
                    break;
                }
                unique_members.insert(member.get<int64_t>());
            }
            group_consistent = valid_members && world > 0 && members.size() == static_cast<size_t>(world) && unique_members.size() == members.size()
                               && rank >= 0 && rank < world && members.at(static_cast<size_t>(rank)) == descriptor["global_rank"];
        }
        if (!group_consistent) ++issues["missing_or_inconsistent_group"];

        nlohmann::json tensor_bytes = nullptr;
        const auto & shape = descriptor["input_shape"];
        const auto & stride = descriptor["input_stride"];
        if (shape.is_array() && stride.is_array() && shape.size() == stride.size() && descriptor["element_bytes"].is_number_integer()
            && descriptor["element_bytes"].get<int64_t>() > 0
            && std::ranges::all_of(shape, [](const auto & value) { return value.is_number_integer() && value.template get<int64_t>() >= 0; })
            && std::ranges::all_of(stride, [](const auto & value) { return value.is_number_integer() && value.template get<int64_t>() >= 0; })) {
            uint64_t bytes = descriptor["element_bytes"].get<uint64_t>();
            for (const auto & dimension : shape) {
                bytes = core::checked_multiply_u64(bytes, dimension.get<uint64_t>(), "collective tensor byte count overflow");
            }
            tensor_bytes = bytes;
        }
        if (tensor_bytes.is_null()) ++issues["missing_or_invalid_tensor_geometry"];

        auto region = observe_submitted_call_region(source, queues, i);
        const bool owned = region.caller.status == "ready" && !region.caller.has_node_overlap;
        if (!owned) ++issues["unresolved_submission_ownership"];
        auto devices = nlohmann::json::array();
        if (owned) {
            for (const auto node : region.submitted_nodes) {
                if (graph.node(node).is_cpu) continue;
                devices.push_back({
                    {    "source_node",                            node },
                    { "implementation", graph.event_for_node(node).name }
                });
                device_owners[node].push_back(calls.size());
            }
        }
        if (devices.empty()) ++issues["no_attributed_device_work"];

        nlohmann::json phase = nullptr;
        const auto owner = owned ? call_phase_position(region, positions) : std::nullopt;
        if (owner) {
            const auto & observation = phases.observations.at(owner->phase_index);
            phase = {
                {    "logical_input",                      observation.logical_input },
                {      "request_ids",                        observation.request_ids },
                {            "phase", owner->decode_iteration ? "decode" : "prefill" },
                { "decode_iteration",                                        nullptr }
            };
            if (owner->decode_iteration) phase["decode_iteration"] = *owner->decode_iteration;
        }
        else { ++issues["missing_or_ambiguous_phase"]; }
        if (event.arg("status") != "returned") ++issues["call_not_returned"];

        auto task_rows = nlohmann::json::array();
        const auto tasks = observe_queued_call_tasks(source, queues, region);
        if (tasks) {
            for (const auto & observed : *tasks) {
                const auto & task = queues.tasks.at(observed.task);
                task_rows.push_back({
                    {     "submission",                            task.submission },
                    { "implementation", graph.event_for_node(task.submission).name },
                    {   "worker_nodes",                           observed.workers },
                    {   "device_nodes",                           observed.devices }
                });
            }
        }

        calls.push_back({
            {                    "observation",                            i },
            {                     "descriptor",        std::move(descriptor) },
            {               "group_consistent",             group_consistent },
            {           "logical_tensor_bytes",                 tensor_bytes },
            {                 "phase_position",             std::move(phase) },
            {                  "caller_status",         region.caller.status },
            {                   "caller_nodes", region.caller.owned_node_ids },
            {                   "device_nodes",           std::move(devices) },
            {            "shared_device_nodes",      nlohmann::json::array() },
            {                   "queued_tasks",         std::move(task_rows) },
            { "queued_task_ownership_complete",            tasks.has_value() }
        });
        regions.push_back(std::move(region));
        owners.push_back(owner);
    }

    for (const auto & [node, owners] : device_owners) {
        if (owners.size() < 2) continue;
        ++issues["multiply_owned_device_node"];
        for (const auto row : owners) calls[row]["shared_device_nodes"].push_back(node);
    }

    auto peer_groups = collective_peer_groups(calls, source);
    const auto unresolved = std::ranges::count_if(peer_groups, [](const auto & group) { return group.at("descriptor_correspondence") != "corroborated"; });
    if (unresolved) issues["unresolved_peer_group"] = unresolved;
    for (const auto & group : peer_groups) {
        const auto & unavailable = group.at("unavailable_completion_boundaries");
        if (!unavailable.is_null() && unavailable.get<size_t>() > 0) issues["unavailable_peer_completion_boundary"] += unavailable.get<size_t>();
        for (const auto & link : group.at("source_peer_completion_links")) {
            if (!link.at("explicit_hccl_edge").get<bool>()) ++issues["missing_explicit_peer_completion_edge"];
        }
    }

    DeviceCollectiveAudit audit;
    for (const auto & group : peer_groups) {
        if (group.at("descriptor_correspondence") != "corroborated") continue;
        const auto rows = group.at("call_rows").get<std::vector<size_t>>();
        const auto & first = calls.at(rows.front()).at("descriptor");
        const auto & shape = first.at("input_shape");
        const bool gather = first.at("implementation") == "sglang.NpuCommunicator.all_gather";
        const bool ordinary = (gather ? first.at("operation") == "gather" : first.at("operation") == "sum") && first.at("input_device_type") == "npu"
                              && first.at("model_layer_implementation") == "sglang.srt.models.qwen3.Qwen3DecoderLayer";
        const bool packed_matrix = shape.size() == 2 && shape.at(0).get<uint64_t>() > 0 && shape.at(1).get<uint64_t>() > 0
                                   && first.at("input_stride") == nlohmann::json::array({ shape.at(1), 1 });
        const bool layer_known = first.at("model_layer_index").is_number_integer() && first.at("model_layer_index").get<int64_t>() >= 0;
        if (!ordinary || !packed_matrix || !layer_known) {
            ++issues["collective_target_layout_unavailable"];
            continue;
        }

        const auto members = first.at("group_members").get<std::vector<int64_t>>();
        if (gather) {
            const auto width = core::checked_multiply_u64(shape.at(1).get<uint64_t>(), members.size(), "Gather output width overflow");
            if (shape.at(0) != 1 || first.at("gather_dim") != 1 || first.at("last_token_projection") != true || first.at("dense_role") != "logits_projection"
                || first.at("callsite") != "_get_logits" || first.at("output_shape") != nlohmann::json::array({ 1, width })
                || first.at("output_stride") != nlohmann::json::array({ width, 1 }) || first.at("output_dtype") != first.at("input_dtype")) {
                ++issues["collective_target_layout_unavailable"];
                continue;
            }
        }
        std::map<int64_t, size_t> rank_observations;
        for (const auto row : rows) rank_observations.emplace(calls.at(row).at("descriptor").at("global_rank").get<int64_t>(), regions.at(row).observation);
        std::vector<size_t> peers;
        for (const auto rank : members) peers.push_back(rank_observations.at(rank));
        for (const auto row : rows) {
            const auto & descriptor = calls.at(row).at("descriptor");
            const auto position = owners.at(row).value();
            const auto & phase = phases.observations.at(position.phase_index);
            const auto & collective_nodes = position.decode_iteration ? phase.decode.collective_node_ids : phase.prefill.collective_node_ids;
            const std::set<size_t> submitted(regions.at(row).submitted_nodes.begin(), regions.at(row).submitted_nodes.end());
            std::vector<size_t> communication_nodes;
            for (const auto node : collective_nodes)
                if (submitted.contains(node)) communication_nodes.push_back(node);
            audit.calls.push_back({ std::move(regions.at(row)),
                                    owners.at(row).value(),
                                    descriptor.at("group").get<std::string>(),
                                    descriptor.at("input_dtype").get<std::string>(),
                                    descriptor.at("dense_role").get<std::string>(),
                                    descriptor.at("callsite").get<std::string>(),
                                    descriptor.at("observed_collective_index").get<uint64_t>(),
                                    descriptor.at("model_layer_index").get<uint64_t>(),
                                    shape.at(0).get<uint64_t>(),
                                    shape.at(1).get<uint64_t>(),
                                    descriptor.at("element_bytes").get<uint64_t>(),
                                    descriptor.at("global_rank").get<int64_t>(),
                                    members,
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
            // Use the raw event, not HCCL's normalized graph duration. Strict
            // microsecond separation avoids inferring order from equal ticks.
            const auto completed = core::checked_add_u64(device.ts, device.dur, "Collective completion overflow");
            if (returned < completed) nonblocking_groups.emplace(call.group, call.global_rank);
        }
    }
    for (auto & call : audit.calls) call.nonblocking_group = nonblocking_groups.contains({ call.group, call.global_rank });

    std::set<size_t> attributed;
    for (const auto & call : audit.calls) attributed.insert(call.region.submitted_nodes.begin(), call.region.submitted_nodes.end());
    for (const auto & phase : phases.observations) {
        for (const auto * collective_nodes : { &phase.prefill.collective_node_ids, &phase.decode.collective_node_ids }) {
            for (const auto node : *collective_nodes)
                if (!attributed.contains(node)) ++audit.issues["phase_collective_without_attributed_call"];
        }
    }
    // Capture/setup observations and diagnostic direct-edge checks are not
    // prediction preconditions. Gate only missing ownership of actual phase
    // collectives; the detailed report retains the other evidence separately.
    for (const auto & [issue, count] : audit.issues) issues[issue] += count;

    const auto status = calls.empty() ? "unavailable" : issues.empty() ? "observed" : "incomplete";
    nlohmann::json report = {
        { "scope", "source ordinary device collective descriptors and submitted work" },
        { "status", status },
        { "calls", std::move(calls) },
        { "attributed_ordinary_calls", audit.calls.size() },
        { "unmodeled_calls", std::move(unmodeled_calls) },
        { "calls_with_nonblocking_group_evidence", std::ranges::count_if(audit.calls, [](const auto & call) { return call.nonblocking_group; }) },
        { "peer_groups", std::move(peer_groups) },
        { "issues", std::move(issues) },
        { "cross_rank_dependencies_proven", false },
        { "transport_cost_available", false },
        { "limitation",
         "Logical tensor bytes are not wire traffic. Local group indices do not prove collective matching; device durations may include peer waits." }
    };
    return { std::move(audit), std::move(report) };
}

} // namespace

DeviceCollectiveAudit observe_device_collectives(const core::DagGraph & graph, const modules::hicache::HiCachePhaseObservationAudit & phases,
                                                 nlohmann::json * diagnostics) {
    auto observed = collect_device_collectives(graph, phases);
    if (diagnostics) *diagnostics = std::move(observed.report);
    return std::move(observed.audit);
}

} // namespace markov::trace_graph::source_dag
