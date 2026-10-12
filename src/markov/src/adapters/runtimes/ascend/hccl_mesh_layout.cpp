#include "markov/adapters/runtimes/ascend/hccl_mesh_layout.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace markov {

std::expected<CommunicationWorkLayout, std::string> HcclMeshLayout::bind(const SourceCollective & source, bool has_serial_completion) const {
    const bool reduce = source.name.starts_with("hcom_allReduce_");
    const bool gather = source.name.starts_with("hcom_allGather_");
    if ((!reduce && !gather) || source.data_type != "BFP16" || !source.communicator) return std::unexpected("mesh_observation_not_supported");
    const auto & config = *source.communicator;
    if (!config.device_type || !config.device_type->starts_with("910B") || config.hcomm_version != "9.0.0" || config.rank_count != 2 || config.server_count != 1
        || config.module_count != 1 || config.deterministic != 0 || config.op_expansion_mode != 0 || source.rank >= 2 || !source.algorithm.starts_with("MESH-")
        || config.transports.empty())
        return std::unexpected("mesh_deployment_not_supported");
    // DEFAULT preserves environment modes; it does not imply host execution.
    if (config.env_aicpu_unfold != 0 || config.env_aiv_mode != 0) return std::unexpected("mesh_expansion_not_supported");

    bool linked = false;
    for (const auto & link : config.transports) {
        if (link.operation != (reduce ? "AllReduce" : "AllGather")) continue;
        if (link.peer != (source.rank ^ 1U) || link.transport != "HCCS") return std::unexpected("mesh_transport_not_supported");
        linked = true;
    }
    if (!linked) return std::unexpected("mesh_transport_not_observed");
    if (!has_serial_completion) return std::unexpected("mesh_task_boundary_not_supported");

    // HCOMM 9.0 MeshTopoSelector uses bytes, not model token counts. The
    // OP_BASE loop capacity is buffer_bytes / sizeof(BF16) * rank_count.
    constexpr std::uint64_t small_bytes = 256 * 1'024;
    constexpr std::uint64_t alignment = 16'384; // HCCL_MIN_SLICE_ALIGN_910B.
    // MeshGather reserves alignment slack; unlike Reduce it stages one full input.
    if (gather && config.buffer_bytes <= alignment) return std::unexpected("mesh_gather_buffer_too_small");
    const auto capacity = reduce ? config.buffer_bytes / 2 * 2 : (config.buffer_bytes - alignment) / 128 * 128 / 2;
    if (source.count == 0 || source.count > capacity || source.count > std::numeric_limits<std::uint64_t>::max() / 2)
        return std::unexpected("mesh_single_loop_domain_exceeded");
    const auto source_bytes = source.count * 2;
    const bool small = reduce && source_bytes <= small_bytes;

    // SmallCount stages the full payload, while OpbaseLoop uses rank slices.
    if (small && source_bytes > config.buffer_bytes) return std::unexpected("mesh_small_buffer_exceeded");

    // One SDMA data task cannot be scaled beyond the native per-task limit.
    constexpr std::uint64_t sdma_limit = 0x1'0000'0000ULL;
    if (gather && source_bytes > sdma_limit) return std::unexpected("mesh_gather_sdma_domain_exceeded");
    const bool full_payload = gather || small;

    // AllReduce owns aligned rank slices; AllGather retains the full payload.
    const auto source_first = (source.count / alignment + (source.count % alignment != 0)) * alignment;
    const auto source_local = full_payload ? source_bytes : source.rank == 0 ? source_first : source_bytes - source_first;
    std::size_t local_copies = 0, peer_reductions = 0, peer_copies = 0, controls = 0, records = 0, waits = 0;
    std::vector<CommunicationTaskBytes> relations;
    for (std::size_t index = 0; index < source.tasks.size(); ++index) {
        const auto & task = source.tasks[index];
        if (task.source_rank != source.rank) return std::unexpected("mesh_task_rank_disagrees");
        if (task.operation == "Reduce_Inline" && task.data_type == "INT32" && task.bytes == 4
            && task.destination_rank == std::numeric_limits<std::uint32_t>::max()) {
            ++controls;
            continue;
        }
        if ((task.operation == "Notify_Record" || task.operation == "Notify_Wait") && task.bytes == 0 && task.destination_rank == (source.rank ^ 1U)) {
            if (task.operation == "Notify_Record") ++records;
            else ++waits;
            continue;
        }
        if (task.transport != "SDMA") return std::unexpected("mesh_data_transport_not_supported");

        const bool local = task.destination_rank == source.rank;
        if (local && task.operation == "Memcpy") ++local_copies;
        else if (task.destination_rank == (source.rank ^ 1U) && task.operation == "Reduce_Inline") ++peer_reductions;
        else if (task.destination_rank == (source.rank ^ 1U) && task.operation == "Memcpy") ++peer_copies;
        else return std::unexpected("mesh_data_role_not_supported");

        const auto observed = full_payload ? source_bytes : local ? source_local : source_bytes - source_local;
        if (task.bytes != observed) return std::unexpected("mesh_source_work_disagrees");

        // The validated local/peer destination selects exactly the native rank
        // slice. Keep this relation independent of the observed rank number.
        relations.push_back({ index, full_payload ? CommunicationByteExtent::Payload : CommunicationByteExtent::DestinationPartition });
    }
    const bool root_small = small && source.rank == 0;
    if (local_copies != (small && source.rank == 1 ? 0U : 2U) || peer_reductions != (gather || root_small ? 0U : 1U) || peer_copies != (root_small ? 0U : 1U)
        || controls != 2 || records != (full_payload ? 2U : 4U) || waits != records)
        return std::unexpected("mesh_task_roles_incomplete");

    // Export finite count domains and task byte relations. Native names and
    // deployment selection remain producer facts, not consumer branches.
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    std::vector<CommunicationCountDomain> domains{
        { 1, std::min(capacity, maximum / 2), "mesh_single_loop_domain_exceeded" }
    };
    if (reduce) domains.push_back({ small ? 1 : small_bytes / 2 + 1, small ? small_bytes / 2 : maximum, "mesh_branch_change_requires_topology_rule" });
    if (small) domains.push_back({ 1, config.buffer_bytes / 2, "mesh_small_buffer_exceeded" });
    if (gather) domains.push_back({ 1, sdma_limit / 2, "mesh_gather_sdma_domain_exceeded" });
    return CommunicationWorkLayout(reduce ? CommunicationOperation::AllReduce : CommunicationOperation::AllGather,
                                   2,
                                   2,
                                   alignment,
                                   std::move(domains),
                                   std::move(relations));
}

} // namespace markov
