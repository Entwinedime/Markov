#include "markov/templates/template_asset.hpp"

#include "markov/templates/templates.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace markov {
namespace {

using Json = nlohmann::json;

// Addresses are used only during encoding. Files contain table indices; decoded
// references borrow owners restored before this table is populated.
template <class T> class AssetReferences {
public:
    void add(const T & value) {
        indices_.emplace(&value, values_.size());
        values_.push_back(&value);
    }
    Json encode(const T * value) const {
        if (!value) return nullptr;

        return indices_.at(value);
    }
    const T * decode(const Json & value) const {
        if (value.is_null()) return nullptr;
        return values_.at(value.get<std::size_t>());
    }

private:
    std::map<const T *, std::size_t> indices_;
    std::vector<const T *> values_;
};

} // namespace

// Positional records keep the large topology arrays compact. Each concrete
// record below lists its fields explicitly; this is the sole current format.
class TemplateAssetCodec {
public:
    Json encode(const TemplateSource & source);
    TemplateSource decode(const Json & data);

private:
    template <class T> Json write(const T & value) const { return Json(value); }
    template <class T> T read(const Json & value) const { return read(value, std::type_identity<T>{}); }
    template <class T> T read(const Json & value, std::type_identity<T>) const { return value.get<T>(); }

    template <class... T> Json record(const T &... values) const {
        auto result = Json::array();
        (result.push_back(write(values)), ...);

        return result;
    }
    template <class T> Json write(const std::vector<T> & values) const {
        auto result = Json::array();
        for (const auto & value : values) result.push_back(write(value));

        return result;
    }
    template <class T> std::vector<T> read(const Json & data, std::type_identity<std::vector<T>>) const {
        std::vector<T> result;
        result.reserve(data.size());
        for (const auto & value : data) result.push_back(read<T>(value));

        return result;
    }
    template <class T> Json write(const std::optional<T> & value) const {
        if (!value) return nullptr;

        return write(*value);
    }
    template <class T> std::optional<T> read(const Json & data, std::type_identity<std::optional<T>>) const {
        if (data.is_null()) return std::nullopt;

        return read<T>(data);
    }
    template <class T, class E> Json write(const std::expected<T, E> & value) const {
        if (value) return record(true, *value);
        return record(false, value.error());
    }
    template <class T, class E> std::expected<T, E> read(const Json & data, std::type_identity<std::expected<T, E>>) const {
        if (data.at(0).get<bool>()) return read<T>(data.at(1));
        return std::unexpected(read<E>(data.at(1)));
    }
    template <class K, class V> Json write(const std::map<K, V> & values) const {
        auto result = Json::array();
        for (const auto & [key, value] : values) result.push_back(record(key, value));

        return result;
    }
    template <class K, class V> std::map<K, V> read(const Json & data, std::type_identity<std::map<K, V>>) const {
        std::map<K, V> result;
        for (const auto & row : data) result.emplace(read<K>(row.at(0)), read<V>(row.at(1)));

        return result;
    }
    Json write(const ModelParameterRule * value) const { return model_rules_.encode(value); }
    Json write(const WorkspaceDemandLayout * value) const { return workspace_rules_.encode(value); }
    const WorkspaceDemandLayout * read(const Json & data, std::type_identity<const WorkspaceDemandLayout *>) const { return workspace_rules_.decode(data); }
    const ModelParameterRule * read(const Json & data, std::type_identity<const ModelParameterRule *>) const { return model_rules_.decode(data); }
    Json write(const CollectiveParameterRule * value) const { return collective_rules_.encode(value); }
    const CollectiveParameterRule * read(const Json & data, std::type_identity<const CollectiveParameterRule *>) const {
        return collective_rules_.decode(data);
    }
    Json write(const TemplateRegionStructure * value) const { return structures_.encode(value); }
    const TemplateRegionStructure * read(const Json & data, std::type_identity<const TemplateRegionStructure *>) const { return structures_.decode(data); }
    Json write(const SourceKernel * value) const { return kernels_.encode(value); }
    const SourceKernel * read(const Json & data, std::type_identity<const SourceKernel *>) const { return kernels_.decode(data); }
    Json write(const SourceTransfer * value) const { return transfers_.encode(value); }
    const SourceTransfer * read(const Json & data, std::type_identity<const SourceTransfer *>) const { return transfers_.decode(data); }
    Json write(const SourceCommunicationTask * value) const { return tasks_.encode(value); }
    const SourceCommunicationTask * read(const Json & data, std::type_identity<const SourceCommunicationTask *>) const { return tasks_.decode(data); }

    Json write(const Environment & value) const { return record(value.service, value.devices); }
    Environment read(const Json & data, std::type_identity<Environment>) const {
        return { read<std::string>(data.at(0)), read<std::vector<std::uint32_t>>(data.at(1)) };
    }

    Json write(const AdmissionPolicy & value) const {
        return record(value.initial_ratio, value.minimum_ratio, value.decay_per_decode, value.output_estimate_limit_tokens, value.reserve_tokens_per_request);
    }
    AdmissionPolicy read(const Json & data, std::type_identity<AdmissionPolicy>) const {
        return { read<double>(data.at(0)),
                 read<double>(data.at(1)),
                 read<double>(data.at(2)),
                 read<std::uint32_t>(data.at(3)),
                 read<std::uint32_t>(data.at(4)) };
    }

    Json write(const BatchTransitionState & value) const { return record(value.running_requests, value.pending_prefill_requests, value.admission_blocked); }
    BatchTransitionState read(const Json & data, std::type_identity<BatchTransitionState>) const {
        return { read<std::size_t>(data.at(0)), read<std::optional<std::size_t>>(data.at(1)), read<bool>(data.at(2)) };
    }

    Json write(const AdmissionEntryState & value) const { return record(value.waiting_requests, value.running_requests, value.admission_blocked); }
    AdmissionEntryState read(const Json & data, std::type_identity<AdmissionEntryState>) const {
        return { read<std::size_t>(data.at(0)), read<std::size_t>(data.at(1)), read<bool>(data.at(2)) };
    }

    Json write(const AdmissionWork & value) const { return record(value.candidate_checks, value.request_horizon_checks, value.stop); }
    AdmissionWork read(const Json & data, std::type_identity<AdmissionWork>) const {
        return { read<std::size_t>(data.at(0)), read<std::vector<std::size_t>>(data.at(1)), read<AdmissionStop>(data.at(2)) };
    }

    Json write(const BatchFilter & value) const { return record(value.input_count, value.retained_indices); }
    BatchFilter read(const Json & data, std::type_identity<BatchFilter>) const {
        return { read<std::size_t>(data.at(0)), read<std::vector<std::size_t>>(data.at(1)) };
    }

    Json write(const BatchMerge & value) const { return record(value.left_count, value.right_count); }
    BatchMerge read(const Json & data, std::type_identity<BatchMerge>) const { return { read<std::size_t>(data.at(0)), read<std::size_t>(data.at(1)) }; }

    Json write(const SourceLane & value) const { return record(value.domain, value.rank, value.native_id); }
    SourceLane read(const Json & data, std::type_identity<SourceLane>) const {
        return { read<SourceDomain>(data.at(0)), read<std::uint32_t>(data.at(1)), read<std::uint64_t>(data.at(2)) };
    }

    Json write(const SourceTransfer & value) const { return record(value.bytes, value.direction, value.source_api); }
    SourceTransfer read(const Json & data, std::type_identity<SourceTransfer>) const {
        return { read<std::uint64_t>(data.at(0)), read<TransferDirection>(data.at(1)), read<std::size_t>(data.at(2)) };
    }

    Json write(const KvSequence & value) const { return record(value.input_tokens, value.prefix_tokens, value.request_pool_slot, value.kv_pages); }
    KvSequence read(const Json & data, std::type_identity<KvSequence>) const {
        return { read<std::int64_t>(data.at(0)),
                 read<std::int64_t>(data.at(1)),
                 read<std::uint32_t>(data.at(2)),
                 read<std::vector<std::uint32_t>>(data.at(3)) };
    }

    Json write(const AttentionWorkload & value) const {
        return record(value.operation, value.sequences, value.query_heads, value.kv_heads, value.head_dim, value.page_size_tokens, value.cache_pages);
    }
    AttentionWorkload read(const Json & data, std::type_identity<AttentionWorkload>) const {
        return { read<AttentionOperation>(data.at(0)), read<std::vector<KvSequence>>(data.at(1)),
                 read<std::int64_t>(data.at(2)),       read<std::int64_t>(data.at(3)),
                 read<std::int64_t>(data.at(4)),       read<std::int64_t>(data.at(5)),
                 read<std::int64_t>(data.at(6)) };
    }

    Json write(const KvAllocationWorkload & value) const { return record(value.page_size_tokens, value.free_pages_before, value.sequences); }
    KvAllocationWorkload read(const Json & data, std::type_identity<KvAllocationWorkload>) const {
        return { read<std::int64_t>(data.at(0)), read<std::uint64_t>(data.at(1)), read<std::vector<KvSequence>>(data.at(2)) };
    }

    Json write(const PageTableWorkload & value) const { return record(value.page_size_tokens, value.sequences); }
    PageTableWorkload read(const Json & data, std::type_identity<PageTableWorkload>) const {
        return { read<std::int64_t>(data.at(0)), read<std::vector<KvSequence>>(data.at(1)) };
    }

    Json write(const KvReleaseWorkload & value) const { return record(value.page_size_tokens, value.free_pages_before, value.sequences); }
    KvReleaseWorkload read(const Json & data, std::type_identity<KvReleaseWorkload>) const {
        return { read<std::int64_t>(data.at(0)), read<std::uint64_t>(data.at(1)), read<std::vector<KvSequence>>(data.at(2)) };
    }

    Json write(const PositionSequence & value) const { return record(value.prefix_tokens, value.input_tokens); }
    PositionSequence read(const Json & data, std::type_identity<PositionSequence>) const {
        return { read<std::int64_t>(data.at(0)), read<std::int64_t>(data.at(1)) };
    }

    Json write(const PositionWorkload & value) const { return record(value.sequences); }
    PositionWorkload read(const Json & data, std::type_identity<PositionWorkload>) const { return { read<std::vector<PositionSequence>>(data.at(0)) }; }

    Json write(const SourceKernel & value) const {
        return record(value.name,
                      value.operation,
                      value.input_shapes,
                      value.output_shapes,
                      value.input_dtypes,
                      value.output_dtypes,
                      value.input_formats,
                      value.output_formats,
                      value.logits_token_indices,
                      value.positions,
                      value.kv_allocation,
                      value.kv_release,
                      value.batch_filter,
                      value.batch_merge,
                      value.page_table,
                      value.sequence_length_max,
                      value.sequence_lengths,
                      value.kv_new_pages,
                      value.request_pool_slots,
                      value.attention);
    }
    SourceKernel read(const Json & data, std::type_identity<SourceKernel>) const {
        return { read<std::string>(data.at(0)),
                 read<std::string>(data.at(1)),
                 read<std::string>(data.at(2)),
                 read<std::string>(data.at(3)),
                 read<std::string>(data.at(4)),
                 read<std::string>(data.at(5)),
                 read<std::string>(data.at(6)),
                 read<std::string>(data.at(7)),
                 read<std::optional<std::vector<std::int64_t>>>(data.at(8)),
                 read<std::optional<PositionWorkload>>(data.at(9)),
                 read<std::optional<KvAllocationWorkload>>(data.at(10)),
                 read<std::optional<KvReleaseWorkload>>(data.at(11)),
                 read<std::optional<BatchFilter>>(data.at(12)),
                 read<std::optional<BatchMerge>>(data.at(13)),
                 read<std::optional<PageTableWorkload>>(data.at(14)),
                 read<std::optional<std::int64_t>>(data.at(15)),
                 read<std::optional<std::vector<std::int64_t>>>(data.at(16)),
                 read<std::optional<std::int64_t>>(data.at(17)),
                 read<std::optional<std::vector<std::uint32_t>>>(data.at(18)),
                 read<std::optional<AttentionWorkload>>(data.at(19)) };
    }

    Json write(const SourceCommunicationTask & value) const {
        return record(value.span,
                      value.source_detail,
                      value.stream,
                      value.task,
                      value.context,
                      value.operation,
                      value.bytes,
                      value.data_type,
                      value.transport,
                      value.source_rank,
                      value.destination_rank);
    }
    SourceCommunicationTask read(const Json & data, std::type_identity<SourceCommunicationTask>) const {
        return { read<SourceSpanId>(data.at(0)),  read<std::size_t>(data.at(1)),   read<std::uint64_t>(data.at(2)), read<std::uint64_t>(data.at(3)),
                 read<std::uint64_t>(data.at(4)), read<std::string>(data.at(5)),   read<std::uint64_t>(data.at(6)), read<std::string>(data.at(7)),
                 read<std::string>(data.at(8)),   read<std::uint32_t>(data.at(9)), read<std::uint32_t>(data.at(10)) };
    }

    Json write(const SourceTransport & value) const { return record(value.operation, value.peer, value.transport); }
    SourceTransport read(const Json & data, std::type_identity<SourceTransport>) const {
        return { read<std::string>(data.at(0)), read<std::uint32_t>(data.at(1)), read<std::string>(data.at(2)) };
    }

    Json write(const SourceCommunicator & value) const {
        return record(value.buffer_bytes,
                      value.deterministic,
                      value.op_expansion_mode,
                      value.rank_count,
                      value.server_count,
                      value.module_count,
                      value.device_type,
                      value.hcomm_version,
                      value.env_aicpu_unfold,
                      value.env_aiv_mode,
                      value.transports);
    }
    SourceCommunicator read(const Json & data, std::type_identity<SourceCommunicator>) const {
        return { read<std::uint64_t>(data.at(0)),
                 read<std::uint32_t>(data.at(1)),
                 read<std::uint32_t>(data.at(2)),
                 read<std::uint32_t>(data.at(3)),
                 read<std::uint32_t>(data.at(4)),
                 read<std::uint32_t>(data.at(5)),
                 read<std::optional<std::string>>(data.at(6)),
                 read<std::optional<std::string>>(data.at(7)),
                 read<std::optional<std::uint32_t>>(data.at(8)),
                 read<std::optional<std::uint32_t>>(data.at(9)),
                 read<std::vector<SourceTransport>>(data.at(10)) };
    }

    Json write(const SourceCollective & value) const {
        return record(value.span,
                      value.rank,
                      value.connection,
                      value.name,
                      value.count,
                      value.data_type,
                      value.algorithm,
                      value.group_name,
                      value.communicator,
                      value.tasks);
    }
    SourceCollective read(const Json & data, std::type_identity<SourceCollective>) const {
        return { read<SourceSpanId>(data.at(0)),
                 read<std::uint32_t>(data.at(1)),
                 read<std::uint64_t>(data.at(2)),
                 read<std::string>(data.at(3)),
                 read<std::uint64_t>(data.at(4)),
                 read<std::string>(data.at(5)),
                 read<std::string>(data.at(6)),
                 read<std::string>(data.at(7)),
                 read<std::optional<SourceCommunicator>>(data.at(8)),
                 read<std::vector<SourceCommunicationTask>>(data.at(9)) };
    }

    Json write(const SourceRequest & value) const {
        return record(value.id, value.input_tokens, value.context_tokens, value.prompt_tokens, value.generated_tokens);
    }
    SourceRequest read(const Json & data, std::type_identity<SourceRequest>) const {
        return { read<std::string>(data.at(0)),
                 read<std::uint32_t>(data.at(1)),
                 read<std::uint32_t>(data.at(2)),
                 read<std::uint32_t>(data.at(3)),
                 read<std::uint32_t>(data.at(4)) };
    }

    Json write(const SourceTensorLayout & value) const { return record(value.shape, value.stride, value.dtype, value.device); }
    SourceTensorLayout read(const Json & data, std::type_identity<SourceTensorLayout>) const {
        return { read<std::vector<std::int64_t>>(data.at(0)),
                 read<std::vector<std::int64_t>>(data.at(1)),
                 read<std::string>(data.at(2)),
                 read<std::string>(data.at(3)) };
    }

    Json write(const SourceNativeRegion & value) const {
        return record(value.semantic,
                      value.module_path,
                      value.module_type,
                      value.method,
                      value.parameters,
                      value.call_parameters,
                      value.inputs,
                      value.outputs,
                      value.cpu_values,
                      value.request_id,
                      value.result);
    }
    SourceNativeRegion read(const Json & data, std::type_identity<SourceNativeRegion>) const {
        return { read<std::string>(data.at(0)),
                 read<std::string>(data.at(1)),
                 read<std::string>(data.at(2)),
                 read<std::string>(data.at(3)),
                 read<std::map<std::string, std::int64_t>>(data.at(4)),
                 read<std::map<std::string, std::int64_t>>(data.at(5)),
                 read<std::map<std::string, std::optional<SourceTensorLayout>>>(data.at(6)),
                 read<std::map<std::string, std::optional<SourceTensorLayout>>>(data.at(7)),
                 read<std::map<std::string, std::vector<std::int64_t>>>(data.at(8)),
                 read<std::optional<std::string>>(data.at(9)),
                 read<nlohmann::json>(data.at(10)) };
    }

    Json write(const TensorParameterAxis & value) const { return record(value.slot, value.multiplier, value.extent, value.axis); }
    TensorParameterAxis read(const Json & data, std::type_identity<TensorParameterAxis>) const {
        return { read<std::size_t>(data.at(0)), read<std::int64_t>(data.at(1)), read<TensorAxisExtent>(data.at(2)), read<std::size_t>(data.at(3)) };
    }

    Json write(const CommunicationCountDomain & value) const { return record(value.minimum, value.maximum, value.gap); }
    CommunicationCountDomain read(const Json & data, std::type_identity<CommunicationCountDomain>) const {
        return { read<std::uint64_t>(data.at(0)), read<std::uint64_t>(data.at(1)), read<std::string>(data.at(2)) };
    }

    Json write(const CommunicationTaskBytes & value) const { return record(value.task, value.extent); }
    CommunicationTaskBytes read(const Json & data, std::type_identity<CommunicationTaskBytes>) const {
        return { read<std::size_t>(data.at(0)), read<CommunicationByteExtent>(data.at(1)) };
    }

    Json write(const CollectiveCountRule & value) const { return record(value.rows, value.width); }
    CollectiveCountRule read(const Json & data, std::type_identity<CollectiveCountRule>) const {
        return { read<CollectiveRows>(data.at(0)), read<std::uint64_t>(data.at(1)) };
    }

    Json write(const CollectiveParameterRule & value) const { return record(value.count, value.work); }
    CollectiveParameterRule read(const Json & data, std::type_identity<CollectiveParameterRule>) const {
        return { read<std::optional<CollectiveCountRule>>(data.at(0)), read<std::expected<CommunicationWorkLayout, std::string>>(data.at(1)) };
    }

    Json write(const ModelParameterObservation & value) const { return record(value.node, value.annotations); }
    ModelParameterObservation read(const Json & data, std::type_identity<ModelParameterObservation>) const {
        return { read<std::size_t>(data.at(0)), read<nlohmann::json>(data.at(1)) };
    }

    Json write(const BatchOperationVariant & value) const {
        return record(value.role, value.source_requests, value.minimum_requests, value.maximum_requests, value.page_table, value.page_size_tokens);
    }
    BatchOperationVariant read(const Json & data, std::type_identity<BatchOperationVariant>) const {
        return { read<BatchOperationRole>(data.at(0)),
                 read<std::uint64_t>(data.at(1)),
                 read<std::uint64_t>(data.at(2)),
                 read<std::uint64_t>(data.at(3)),
                 read<std::optional<PageTableMaterialization>>(data.at(4)),
                 read<std::uint32_t>(data.at(5)) };
    }

    Json write(const BatchRowParameter & value) const { return record(value.output, value.slot, value.extent); }
    BatchRowParameter read(const Json & data, std::type_identity<BatchRowParameter>) const {
        return { read<bool>(data.at(0)), read<std::size_t>(data.at(1)), read<BatchRowExtent>(data.at(2)) };
    }

    Json write(const BatchRequestBinding & value) const { return record(value.request_count, value.slot); }
    BatchRequestBinding read(const Json & data, std::type_identity<BatchRequestBinding>) const {
        return { read<std::size_t>(data.at(0)), read<std::size_t>(data.at(1)) };
    }

    Json write(const KvParameterAxis & value) const { return record(value.output, value.slot, value.extent); }
    KvParameterAxis read(const Json & data, std::type_identity<KvParameterAxis>) const {
        return { read<bool>(data.at(0)), read<std::size_t>(data.at(1)), read<KvParameterExtent>(data.at(2)) };
    }

    Json write(const KvParameterDomain & value) const {
        return record(value.maximum_new_pages, value.allocates_pages, value.release_free_empty, value.release_request_slot);
    }
    KvParameterDomain read(const Json & data, std::type_identity<KvParameterDomain>) const {
        return { read<std::optional<std::int64_t>>(data.at(0)),
                 read<std::optional<bool>>(data.at(1)),
                 read<std::optional<bool>>(data.at(2)),
                 read<std::optional<std::size_t>>(data.at(3)) };
    }

    Json write(const RequestPoolCapacityDomain & value) const { return record(value.maximum, value.shrink_gap); }
    RequestPoolCapacityDomain read(const Json & data, std::type_identity<RequestPoolCapacityDomain>) const {
        return { read<std::uint32_t>(data.at(0)), read<std::optional<std::string>>(data.at(1)) };
    }

    Json write(const TransferParameterLayout & value) const { return record(value.extent, value.request_count, value.finished_count, value.element_bytes); }
    TransferParameterLayout read(const Json & data, std::type_identity<TransferParameterLayout>) const {
        return { read<TransferExtent>(data.at(0)), read<std::size_t>(data.at(1)), read<std::size_t>(data.at(2)), read<std::uint64_t>(data.at(3)) };
    }

    Json write(const TemplateResource & value) const { return record(value.domain, value.rank, value.native_id, value.role); }
    TemplateResource read(const Json & data, std::type_identity<TemplateResource>) const {
        return { read<SourceDomain>(data.at(0)),
                 read<std::uint32_t>(data.at(1)),
                 read<std::uint64_t>(data.at(2)),
                 read<std::optional<TemplateResourceRole>>(data.at(3)) };
    }

    Json write(const TemplateLane & value) const { return record(value.source_lane, value.source, value.role); }
    TemplateLane read(const Json & data, std::type_identity<TemplateLane>) const {
        return { read<std::size_t>(data.at(0)), read<SourceLane>(data.at(1)), read<std::optional<TemplateResourceRole>>(data.at(2)) };
    }

    Json write(const TemplateBoundary & value) const { return record(value.lane); }
    TemplateBoundary read(const Json & data, std::type_identity<TemplateBoundary>) const { return { read<std::size_t>(data.at(0)) }; }

    Json write(const TemplateBoundaryObservation & value) const { return record(value.source_boundary, value.observed_time_ns); }
    TemplateBoundaryObservation read(const Json & data, std::type_identity<TemplateBoundaryObservation>) const {
        return { read<BoundaryId>(data.at(0)), read<std::int64_t>(data.at(1)) };
    }

    Json write(const WorkspacePartition & value) const {
        return record(value.groups, value.group_size_limit, value.minimum_members, value.maximum_sequence_exclusive, value.group_size_by_members);
    }
    WorkspacePartition read(const Json & data, std::type_identity<WorkspacePartition>) const {
        return { read<std::uint32_t>(data.at(0)),
                 read<std::uint32_t>(data.at(1)),
                 read<std::uint32_t>(data.at(2)),
                 read<std::uint32_t>(data.at(3)),
                 read<std::map<std::uint32_t, std::uint32_t>>(data.at(4)) };
    }
    Json write(const WorkspaceScaledBuffer & value) const { return record(value.scale, value.groups, value.element_bytes); }
    WorkspaceScaledBuffer read(const Json & data, std::type_identity<WorkspaceScaledBuffer>) const {
        return { read<float>(data.at(0)), read<std::uint32_t>(data.at(1)), read<std::uint32_t>(data.at(2)) };
    }
    Json write(const WorkspaceDemandLayout & value) const {
        return record(value.units_, value.bytes_per_unit_, value.alignment_, value.partition_, value.buffers_);
    }
    WorkspaceDemandLayout read(const Json & data, std::type_identity<WorkspaceDemandLayout>) const {
        return { read<std::uint32_t>(data.at(0)),
                 read<std::uint64_t>(data.at(1)),
                 read<std::uint32_t>(data.at(2)),
                 read<std::optional<WorkspacePartition>>(data.at(3)),
                 read<std::vector<WorkspaceScaledBuffer>>(data.at(4)) };
    }

    Json write(const TemplateWorkspacePool & value) const {
        return record(value.resource, value.initial_capacity_bytes, value.allocation_padding_bytes, value.allocation_alignment_bytes);
    }
    TemplateWorkspacePool read(const Json & data, std::type_identity<TemplateWorkspacePool>) const {
        return { read<TemplateResource>(data.at(0)), read<std::uint64_t>(data.at(1)), read<std::uint32_t>(data.at(2)), read<std::uint32_t>(data.at(3)) };
    }
    Json write(const TemplateWorkspaceUse & value) const {
        return record(value.pool,
                      value.operation,
                      value.worker,
                      value.source_api,
                      value.marker,
                      value.begin,
                      value.end,
                      value.requested_bytes,
                      value.capacity_before_bytes,
                      value.capacity_after_bytes,
                      value.branch,
                      value.requires_device_synchronization,
                      value.demand);
    }
    TemplateWorkspaceUse read(const Json & data, std::type_identity<TemplateWorkspaceUse>) const {
        return { read<std::size_t>(data.at(0)),
                 read<std::size_t>(data.at(1)),
                 read<std::size_t>(data.at(2)),
                 read<std::size_t>(data.at(3)),
                 read<SourceSpanId>(data.at(4)),
                 read<std::size_t>(data.at(5)),
                 read<std::size_t>(data.at(6)),
                 read<std::uint64_t>(data.at(7)),
                 read<std::uint64_t>(data.at(8)),
                 read<std::uint64_t>(data.at(9)),
                 read<WorkspaceBranch>(data.at(10)),
                 read<bool>(data.at(11)),
                 read<std::expected<const WorkspaceDemandLayout *, std::string>>(data.at(12)) };
    }

    Json write(const TemplateStreamWait & value) const {
        return record(value.source_span, value.native_stream, value.completion, value.resources, value.scope);
    }
    TemplateStreamWait read(const Json & data, std::type_identity<TemplateStreamWait>) const {
        return { read<SourceSpanId>(data.at(0)),
                 read<std::optional<std::uint64_t>>(data.at(1)),
                 read<std::size_t>(data.at(2)),
                 read<std::vector<TemplateResource>>(data.at(3)),
                 read<SynchronizationScope>(data.at(4)) };
    }

    Json write(const TemplateNode & value) const { return record(value.observation_slot, value.kind, value.begin, value.end); }
    TemplateNode read(const Json & data, std::type_identity<TemplateNode>) const {
        return { read<std::size_t>(data.at(0)), read<SourceSpanKind>(data.at(1)), read<std::size_t>(data.at(2)), read<std::size_t>(data.at(3)) };
    }

    Json write(const TemplateDependency & value) const { return record(value.relation, value.source, value.target); }
    TemplateDependency read(const Json & data, std::type_identity<TemplateDependency>) const {
        return { read<SourceRelation>(data.at(0)), read<std::size_t>(data.at(1)), read<std::size_t>(data.at(2)) };
    }

    Json write(const TemplateConnectionContract & value) const { return record(value.relation, value.direction, value.local_boundary); }
    TemplateConnectionContract read(const Json & data, std::type_identity<TemplateConnectionContract>) const {
        return { read<SourceRelation>(data.at(0)), read<TemplateConnectionDirection>(data.at(1)), read<std::size_t>(data.at(2)) };
    }

    Json write(const TemplateInterface & value) const { return record(value.connection, value.remote_boundary); }
    TemplateInterface read(const Json & data, std::type_identity<TemplateInterface>) const {
        return { read<TemplateConnectionContract>(data.at(0)), read<std::size_t>(data.at(1)) };
    }

    Json write(const TemplateResourceSlot & value) const { return record(value.domain, value.rank); }
    TemplateResourceSlot read(const Json & data, std::type_identity<TemplateResourceSlot>) const {
        return { read<SourceDomain>(data.at(0)), read<std::size_t>(data.at(1)) };
    }

    Json write(const TemplateRemoteBoundary & value) const { return record(value.lane, value.local_boundary); }
    TemplateRemoteBoundary read(const Json & data, std::type_identity<TemplateRemoteBoundary>) const {
        return { read<std::size_t>(data.at(0)), read<std::optional<std::size_t>>(data.at(1)) };
    }

    Json write(const TemplateRegionStructure & value) const {
        return record(value.resources,
                      value.boundaries,
                      value.nodes,
                      value.dependencies,
                      value.connections,
                      value.dependency_ports,
                      value.remote_boundaries,
                      value.collective_rules,
                      value.model_rules);
    }
    TemplateRegionStructure read(const Json & data, std::type_identity<TemplateRegionStructure>) const {
        return { read<std::vector<TemplateResourceSlot>>(data.at(0)),
                 read<std::vector<TemplateBoundary>>(data.at(1)),
                 read<std::vector<TemplateNode>>(data.at(2)),
                 read<std::vector<TemplateDependency>>(data.at(3)),
                 read<std::vector<TemplateInterface>>(data.at(4)),
                 read<std::vector<TemplateInterface>>(data.at(5)),
                 read<std::vector<TemplateRemoteBoundary>>(data.at(6)),
                 read<std::map<std::size_t, const CollectiveParameterRule *>>(data.at(7)),
                 read<std::map<std::size_t, const ModelParameterRule *>>(data.at(8)) };
    }

    Json write(const TemplateConnectionObservation & value) const {
        return record(value.source_edge, value.remote_source_calls, value.resource, value.remote_order);
    }
    TemplateConnectionObservation read(const Json & data, std::type_identity<TemplateConnectionObservation>) const {
        return { read<std::size_t>(data.at(0)),
                 read<std::vector<std::size_t>>(data.at(1)),
                 read<std::optional<TemplateResource>>(data.at(2)),
                 read<std::optional<std::uint64_t>>(data.at(3)) };
    }

    Json write(const TemplateRegion & value) const {
        return record(value.source_call, value.parent_call, value.binding, value.module, value.nodes, value.inputs, value.outputs, value.request_slot);
    }
    TemplateRegion read(const Json & data, std::type_identity<TemplateRegion>) const {
        return { read<std::size_t>(data.at(0)),
                 read<std::size_t>(data.at(1)),
                 read<std::optional<std::string>>(data.at(2)),
                 read<std::optional<SourceNativeRegion>>(data.at(3)),
                 read<std::vector<std::size_t>>(data.at(4)),
                 read<std::vector<std::size_t>>(data.at(5)),
                 read<std::vector<std::size_t>>(data.at(6)),
                 read<std::optional<std::size_t>>(data.at(7)) };
    }

    Json write(const RotaryPositionDependency & value) const { return record(value.producer, value.consumer); }
    RotaryPositionDependency read(const Json & data, std::type_identity<RotaryPositionDependency>) const {
        return { read<std::size_t>(data.at(0)), read<std::size_t>(data.at(1)) };
    }

    Json write(const TemplateOperationWindow & value) const { return record(value.source_span, value.begin, value.end); }
    TemplateOperationWindow read(const Json & data, std::type_identity<TemplateOperationWindow>) const {
        return { read<SourceSpanId>(data.at(0)), read<std::size_t>(data.at(1)), read<std::size_t>(data.at(2)) };
    }

    Json write(const TemplateOperation & value) const {
        return record(value.name, value.host, value.workers, value.device_spans, value.input_shapes, value.input_types, value.collective_spans);
    }
    TemplateOperation read(const Json & data, std::type_identity<TemplateOperation>) const {
        return { read<std::string>(data.at(0)),
                 read<TemplateOperationWindow>(data.at(1)),
                 read<std::vector<TemplateOperationWindow>>(data.at(2)),
                 read<std::vector<SourceSpanId>>(data.at(3)),
                 read<std::optional<std::string>>(data.at(4)),
                 read<std::optional<std::string>>(data.at(5)),
                 read<std::vector<SourceSpanId>>(data.at(6)) };
    }

    Json write(const TemplateCollective & value) const {
        return record(value.observation,
                      value.envelope_node,
                      value.task_nodes,
                      value.entry_boundaries,
                      value.exit_boundaries,
                      value.internal_dependencies,
                      value.completion);
    }
    TemplateCollective read(const Json & data, std::type_identity<TemplateCollective>) const {
        return { read<SourceCollective>(data.at(0)),          read<std::size_t>(data.at(1)),
                 read<std::vector<std::size_t>>(data.at(2)),  read<std::vector<std::size_t>>(data.at(3)),
                 read<std::vector<std::size_t>>(data.at(4)),  read<std::vector<std::size_t>>(data.at(5)),
                 read<std::optional<std::size_t>>(data.at(6)) };
    }

    Json write(const RequestLengthDomain & value) const {
        return {
            {   "minimum_input_tokens",   value.minimum_input_tokens },
            {   "maximum_input_tokens",   value.maximum_input_tokens },
            { "minimum_context_tokens", value.minimum_context_tokens },
            { "maximum_context_tokens", value.maximum_context_tokens }
        };
    }
    RequestLengthDomain read(const Json & data, std::type_identity<RequestLengthDomain>) const {
        return { read<std::uint32_t>(data.at("minimum_input_tokens")),
                 read<std::uint32_t>(data.at("maximum_input_tokens")),
                 read<std::uint32_t>(data.at("minimum_context_tokens")),
                 read<std::uint32_t>(data.at("maximum_context_tokens")) };
    }

    Json write(const KvRegionBranch & value) const {
        return record(value.kind, value.region, value.changes_pages, value.minimum_members, value.maximum_members);
    }
    KvRegionBranch read(const Json & data, std::type_identity<KvRegionBranch>) const {
        return { read<KvRegionKind>(data.at(0)),
                 read<std::size_t>(data.at(1)),
                 read<bool>(data.at(2)),
                 read<std::size_t>(data.at(3)),
                 read<std::size_t>(data.at(4)) };
    }

    Json write(const PeriodicOutputPolicy & value) const { return record(value.interval_tokens); }
    PeriodicOutputPolicy read(const Json & data, std::type_identity<PeriodicOutputPolicy>) const { return { read<std::uint32_t>(data.at(0)) }; }

    Json write(const TemplateGap & value) const { return record(value.kind, value.reason, value.step, value.phase, value.rank); }
    TemplateGap read(const Json & data, std::type_identity<TemplateGap>) const {
        return { read<TemplateGapKind>(data.at(0)),
                 read<std::string>(data.at(1)),
                 read<StepKind>(data.at(2)),
                 read<BatchPhase>(data.at(3)),
                 read<std::uint32_t>(data.at(4)) };
    }

    Json write(const TensorParameterLayout & value) const { return record(value.inputs_, value.outputs_, value.page_size_tokens_); }
    TensorParameterLayout read(const Json & data, std::type_identity<TensorParameterLayout>) const {
        return { read<std::vector<TensorParameterAxis>>(data.at(0)), read<std::vector<TensorParameterAxis>>(data.at(1)), read<std::int64_t>(data.at(2)) };
    }

    Json write(const ModelParameterRule & value) const { return record(value.role_, value.shapes_, value.workload_, value.partial_operands_); }
    ModelParameterRule read(const Json & data, std::type_identity<ModelParameterRule>) const {
        auto shapes = read<TensorParameterLayout>(data.at(1));
        return { read<ModelKernelRole>(data.at(0)), std::move(shapes.inputs_),       std::move(shapes.outputs_),
                 shapes.page_size_tokens_,          read<ModelWorkload>(data.at(2)), read<bool>(data.at(3)) };
    }

    Json write(const CommunicationWorkLayout & value) const {
        return record(value.operation_, value.element_bytes_, value.partitions_, value.alignment_, value.domains_, value.relations_);
    }
    CommunicationWorkLayout read(const Json & data, std::type_identity<CommunicationWorkLayout>) const {
        return { read<CommunicationOperation>(data.at(0)),
                 read<std::uint32_t>(data.at(1)),
                 read<std::uint32_t>(data.at(2)),
                 read<std::uint64_t>(data.at(3)),
                 read<std::vector<CommunicationCountDomain>>(data.at(4)),
                 read<std::vector<CommunicationTaskBytes>>(data.at(5)) };
    }

    Json write(const AttentionParameterRule & value) const { return record(value.shapes_, value.workload_); }
    AttentionParameterRule read(const Json & data, std::type_identity<AttentionParameterRule>) const {
        return { read<TensorParameterLayout>(data.at(0)), read<AttentionWorkload>(data.at(1)) };
    }

    Json write(const AttentionParameterObservation & value) const { return record(attention_rules_.encode(value.rule), value.annotations); }
    AttentionParameterObservation read(const Json & data, std::type_identity<AttentionParameterObservation>) const {
        return { attention_rules_.decode(data.at(0)), read<nlohmann::json>(data.at(1)) };
    }

    Json write(const BatchParameterLayout & value) const { return record(value.filter_, value.rows_, value.request_, value.input_arity_); }
    BatchParameterLayout read(const Json & data, std::type_identity<BatchParameterLayout>) const {
        return { read<std::optional<BatchFilterBinding>>(data.at(0)),
                 read<std::vector<BatchRowParameter>>(data.at(1)),
                 read<std::optional<BatchRequestBinding>>(data.at(2)),
                 read<BatchInputArity>(data.at(3)) };
    }

    Json write(const KvParameterLayout & value) const {
        return record(value.kind_, value.page_size_, value.domain_, value.axes_, value.output_, value.release_member_inputs_);
    }
    KvParameterLayout read(const Json & data, std::type_identity<KvParameterLayout>) const {
        return { read<KvParameterKind>(data.at(0)),   read<std::uint32_t>(data.at(1)),
                 read<KvParameterDomain>(data.at(2)), read<std::vector<KvParameterAxis>>(data.at(3)),
                 read<KvParameterOutput>(data.at(4)), read<bool>(data.at(5)) };
    }

    Json write(const RequestPoolParameterLayout & value) const { return record(value.shapes_, value.indexed_slots_, value.single_request_only_); }
    RequestPoolParameterLayout read(const Json & data, std::type_identity<RequestPoolParameterLayout>) const {
        return { read<TensorParameterLayout>(data.at(0)), read<bool>(data.at(1)), read<bool>(data.at(2)) };
    }

    Json write(const PublicationParameterLayout & value) const {
        return record(value.source_span_, value.interval_tokens_, value.context_limit_, value.sender_enabled_);
    }
    PublicationParameterLayout read(const Json & data, std::type_identity<PublicationParameterLayout>) const {
        return { read<SourceSpanId>(data.at(0)), read<std::uint32_t>(data.at(1)), read<std::uint32_t>(data.at(2)), read<bool>(data.at(3)) };
    }

    Json write(const TemplateGraph::Location & value) const { return record(value.region, value.member); }
    TemplateGraph::Location read(const Json & data, std::type_identity<TemplateGraph::Location>) const {
        return { read<std::size_t>(data.at(0)), read<std::size_t>(data.at(1)) };
    }

    Json write(const TemplateGraph::RemoteBoundaryBinding & value) const { return record(value.source_boundary, value.local_boundary); }
    TemplateGraph::RemoteBoundaryBinding read(const Json & data, std::type_identity<TemplateGraph::RemoteBoundaryBinding>) const {
        return { read<BoundaryId>(data.at(0)), read<std::optional<std::size_t>>(data.at(1)) };
    }

    Json write(const TemplateGraph::ObservedConnection & value) const { return record(value.contract, value.remote_source_boundary, value.remote_lane); }
    TemplateGraph::ObservedConnection read(const Json & data, std::type_identity<TemplateGraph::ObservedConnection>) const {
        return { read<TemplateConnectionContract>(data.at(0)), read<BoundaryId>(data.at(1)), read<SourceLane>(data.at(2)) };
    }

    Json write(const TemplateGraph::Binding & value) const { return record(value.structure, value.boundaries, value.remote_boundaries, value.lanes); }
    TemplateGraph::Binding read(const Json & data, std::type_identity<TemplateGraph::Binding>) const {
        return { read<const TemplateRegionStructure *>(data.at(0)),
                 read<std::vector<std::size_t>>(data.at(1)),
                 read<std::vector<TemplateGraph::RemoteBoundaryBinding>>(data.at(2)),
                 read<std::vector<std::optional<std::size_t>>>(data.at(3)) };
    }

    Json write(const TemplateNodeObservation & value) const {
        return record(value.source_span, value.kernel, value.source_scope, value.communication, value.transfer);
    }
    TemplateNodeObservation read(const Json & data, std::type_identity<TemplateNodeObservation>) const {
        return { read<SourceSpanId>(data.at(0)),
                 read<const SourceKernel *>(data.at(1)),
                 read<std::optional<std::size_t>>(data.at(2)),
                 read<const SourceCommunicationTask *>(data.at(3)),
                 read<const SourceTransfer *>(data.at(4)) };
    }

    Json write(const TemplateGraph & value) const {
        return record(value.local_boundaries_,
                      value.boundary_locations_,
                      value.local_,
                      value.locations_,
                      value.bindings_,
                      value.local_dependencies_,
                      value.dependency_locations_,
                      value.local_connections_,
                      value.connection_locations_,
                      value.model_rules_,
                      value.collective_rules_);
    }
    TemplateGraph read(const Json & data, std::type_identity<TemplateGraph>) const {
        TemplateGraph result;
        result.local_boundaries_ = read<std::vector<TemplateBoundary>>(data.at(0));
        result.boundary_locations_ = read<std::vector<TemplateGraph::Location>>(data.at(1));
        result.local_ = read<std::vector<TemplateNode>>(data.at(2));
        result.locations_ = read<std::vector<TemplateGraph::Location>>(data.at(3));
        result.bindings_ = read<std::vector<TemplateGraph::Binding>>(data.at(4));
        result.local_dependencies_ = read<std::vector<TemplateDependency>>(data.at(5));
        result.dependency_locations_ = read<std::vector<TemplateGraph::Location>>(data.at(6));
        result.local_connections_ = read<std::vector<TemplateGraph::ObservedConnection>>(data.at(7));
        result.connection_locations_ = read<std::vector<TemplateGraph::Location>>(data.at(8));
        result.model_rules_ = read<std::map<std::size_t, const ModelParameterRule *>>(data.at(9));
        result.collective_rules_ = read<std::map<std::size_t, const CollectiveParameterRule *>>(data.at(10));

        return result;
    }

    Json write(const TemplateDefinition & value) {
        tasks_ = {};
        for (const auto & collective : value.collectives)
            for (const auto & task : collective.observation.tasks) tasks_.add(task);

        return {
            {                           "step",                           write(value.step) },
            {                          "phase",                          write(value.phase) },
            {                           "rank",                           write(value.rank) },
            {                    "source_call",                    write(value.source_call) },
            {                   "source_batch",                   write(value.source_batch) },
            {        "source_invocation_order",                   write(value.source_order) },
            {                     "transition",                     write(value.transition) },
            {              "transition_region",              write(value.transition_region) },
            { "transition_latch_parameterized", write(value.transition_latch_parameterized) },
            {      "transition_cpu_parameters",      write(value.transition_cpu_parameters) },
            {    "transition_merge_parameters",    write(value.transition_merge_parameters) },
            {          "running_update_region",          write(value.running_update_region) },
            {                      "admission",                      write(value.admission) },
            {                 "admission_work",                 write(value.admission_work) },
            {    "admission_cpu_request_count",    write(value.admission_cpu_request_count) },
            {              "admission_regions",              write(value.admission_regions) },
            {                        "filters",                        write(value.filters) },
            {                          "merge",                          write(value.merge) },
            {                        "binding",                        write(value.binding) },
            {                 "source_subtree",                 write(value.source_subtree) },
            {                          "entry",                          write(value.entry) },
            {                          "ports",                          write(value.ports) },
            {                   "input_layout",                   write(value.input_layout) },
            {                "source_requests",                write(value.source_requests) },
            {                  "length_domain",                  write(value.length_domain) },
            {           "publication_finishes",           write(value.publication_finishes) },
            {                  "output_policy",                  write(value.output_policy) },
            {             "publication_layout",             write(value.publication_layout) },
            {            "publication_regions",            write(value.publication_regions) },
            { "request_initialization_regions", write(value.request_initialization_regions) },
            {       "publication_cpu_progress",       write(value.publication_cpu_progress) },
            {                          "lanes",                          write(value.lanes) },
            {          "boundary_observations",          write(value.boundary_observations) },
            {                          "graph",                          write(value.graph) },
            {              "node_observations",              write(value.node_observations) },
            {                        "regions",                        write(value.regions) },
            {                      "kv_region",                      write(value.kv_region) },
            {                     "operations",                     write(value.operations) },
            {                 "workspace_uses",                 write(value.workspace_uses) },
            {               "batch_operations",               write(value.batch_operations) },
            {               "transfer_layouts",               write(value.transfer_layouts) },
            {       "logits_reshape_operation",       write(value.logits_reshape_operation) },
            {                    "collectives",                    write(value.collectives) },
            {           "model_kernel_layouts",           write(value.model_kernel_layouts) },
            {       "attention_kernel_layouts",       write(value.attention_kernel_layouts) },
            {              "kv_kernel_layouts",              write(value.kv_kernel_layouts) },
            {           "request_pool_layouts",           write(value.request_pool_layouts) },
            {           "batch_kernel_layouts",           write(value.batch_kernel_layouts) },
            {                   "stream_waits",                   write(value.stream_waits) },
            {   "rotary_position_dependencies",   write(value.rotary_position_dependencies) },
            {        "connection_observations",        write(value.connection_observations) },
            {                         "inputs",                         write(value.inputs) },
            {                        "outputs",                        write(value.outputs) }
        };
    }
    TemplateDefinition definition(const Json & data) {
        TemplateDefinition result(read<StepKind>(data.at("step")),
                                  read<BatchPhase>(data.at("phase")),
                                  read<std::uint32_t>(data.at("rank")),
                                  read<std::size_t>(data.at("source_call")));
        // Communication observations borrow tasks from these owning collectives.
        result.collectives = read<std::vector<TemplateCollective>>(data.at("collectives"));
        tasks_ = {};
        for (const auto & collective : result.collectives)
            for (const auto & task : collective.observation.tasks) tasks_.add(task);

        result.source_batch = read<std::optional<std::uint64_t>>(data.at("source_batch"));
        result.source_order = read<std::uint64_t>(data.at("source_invocation_order"));

        result.transition = read<std::optional<BatchTransitionState>>(data.at("transition"));
        result.transition_region = read<std::optional<std::size_t>>(data.at("transition_region"));
        result.transition_latch_parameterized = read<bool>(data.at("transition_latch_parameterized"));
        result.transition_cpu_parameters = read<bool>(data.at("transition_cpu_parameters"));
        result.transition_merge_parameters = read<bool>(data.at("transition_merge_parameters"));
        result.running_update_region = read<std::optional<std::size_t>>(data.at("running_update_region"));

        result.admission = read<std::optional<AdmissionEntryState>>(data.at("admission"));
        result.admission_work = read<std::optional<AdmissionWork>>(data.at("admission_work"));
        result.admission_cpu_request_count = read<std::optional<std::size_t>>(data.at("admission_cpu_request_count"));
        result.admission_regions = read<std::vector<std::size_t>>(data.at("admission_regions"));
        result.filters = read<std::vector<BatchFilter>>(data.at("filters"));
        result.merge = read<std::optional<BatchMerge>>(data.at("merge"));
        result.binding = read<std::string>(data.at("binding"));
        result.source_subtree = read<std::vector<std::size_t>>(data.at("source_subtree"));

        result.entry = read<std::size_t>(data.at("entry"));
        result.ports = read<std::map<PortKind, std::size_t>>(data.at("ports"));
        result.input_layout = read<std::optional<SourceTensorLayout>>(data.at("input_layout"));
        result.source_requests = read<std::vector<SourceRequest>>(data.at("source_requests"));
        result.length_domain = read<std::optional<RequestLengthDomain>>(data.at("length_domain"));

        result.publication_finishes = read<std::vector<std::optional<bool>>>(data.at("publication_finishes"));
        result.output_policy = read<std::optional<PeriodicOutputPolicy>>(data.at("output_policy"));
        result.publication_layout = read<std::optional<PublicationParameterLayout>>(data.at("publication_layout"));
        result.publication_regions = read<std::vector<std::size_t>>(data.at("publication_regions"));
        result.request_initialization_regions = read<std::vector<std::size_t>>(data.at("request_initialization_regions"));
        result.publication_cpu_progress = read<bool>(data.at("publication_cpu_progress"));

        result.lanes = read<std::vector<TemplateLane>>(data.at("lanes"));
        result.boundary_observations = read<std::vector<TemplateBoundaryObservation>>(data.at("boundary_observations"));
        result.graph = read<TemplateGraph>(data.at("graph"));
        result.node_observations = read<std::vector<TemplateNodeObservation>>(data.at("node_observations"));

        result.regions = read<std::vector<TemplateRegion>>(data.at("regions"));
        result.kv_region = read<std::optional<KvRegionBranch>>(data.at("kv_region"));
        result.operations = read<std::vector<TemplateOperation>>(data.at("operations"));
        result.workspace_uses = read<std::vector<TemplateWorkspaceUse>>(data.at("workspace_uses"));
        result.batch_operations = read<std::map<SourceSpanId, BatchOperationVariant>>(data.at("batch_operations"));
        result.transfer_layouts = read<std::map<SourceSpanId, TransferParameterLayout>>(data.at("transfer_layouts"));
        result.logits_reshape_operation = read<std::optional<std::size_t>>(data.at("logits_reshape_operation"));

        result.model_kernel_layouts = read<std::map<SourceSpanId, ModelParameterObservation>>(data.at("model_kernel_layouts"));
        result.attention_kernel_layouts = read<std::map<SourceSpanId, AttentionParameterObservation>>(data.at("attention_kernel_layouts"));
        result.kv_kernel_layouts = read<std::map<SourceSpanId, std::expected<KvParameterLayout, std::string>>>(data.at("kv_kernel_layouts"));
        result.request_pool_layouts = read<std::map<SourceSpanId, std::expected<RequestPoolParameterLayout, std::string>>>(data.at("request_pool_layouts"));
        result.batch_kernel_layouts = read<std::map<SourceSpanId, std::expected<BatchParameterLayout, std::string>>>(data.at("batch_kernel_layouts"));

        result.stream_waits = read<std::vector<TemplateStreamWait>>(data.at("stream_waits"));
        result.rotary_position_dependencies = read<std::vector<RotaryPositionDependency>>(data.at("rotary_position_dependencies"));
        result.connection_observations = read<std::vector<TemplateConnectionObservation>>(data.at("connection_observations"));
        result.inputs = read<std::vector<BoundaryId>>(data.at("inputs"));
        result.outputs = read<std::vector<BoundaryId>>(data.at("outputs"));

        return result;
    }

    AssetReferences<ModelParameterRule> model_rules_;
    AssetReferences<WorkspaceDemandLayout> workspace_rules_;
    AssetReferences<AttentionParameterRule> attention_rules_;
    AssetReferences<CollectiveParameterRule> collective_rules_;
    AssetReferences<TemplateRegionStructure> structures_;
    AssetReferences<SourceKernel> kernels_;
    AssetReferences<SourceTransfer> transfers_;
    AssetReferences<SourceCommunicationTask> tasks_;
};

Json TemplateAssetCodec::encode(const TemplateSource & source) {
    for (const auto & [span, kernel] : source.kernels) kernels_.add(kernel);
    for (const auto & [span, transfer] : source.transfers) transfers_.add(transfer);

    auto model_rules = Json::array();
    for (const auto & rule : source.model_rules) {
        model_rules_.add(rule);
        model_rules.push_back(write(rule));
    }
    auto collective_rules = Json::array();
    for (const auto & rule : source.collective_rules) {
        collective_rules_.add(rule);
        collective_rules.push_back(write(rule));
    }
    auto workspace_rules = Json::array();
    for (const auto & rule : source.workspace_rules) {
        workspace_rules_.add(rule);
        workspace_rules.push_back(write(rule));
    }
    auto attention_rules = Json::array();
    for (const auto & rule : source.attention_rules) {
        attention_rules_.add(rule);
        attention_rules.push_back(write(rule));
    }
    auto structures = Json::array();
    for (const auto & structure : source.region_structures) {
        structures_.add(structure);
        structures.push_back(write(structure));
    }

    auto definitions = Json::array();
    for (const auto & [key, members] : source.definitions)
        for (const auto & member : members) definitions.push_back(write(member));

    return {
        {            "manifest_path",          source.manifest_path.string() },
        {           "templates_path",         source.templates_path.string() },
        {               "model_path",                      source.model_path },
        {            "configuration",                   source.configuration },
        {                    "ranks",                           source.ranks },
        {         "page_size_tokens",                source.page_size_tokens },
        {               "deployment",               write(source.deployment) },
        {               "resolution",                      source.resolution },
        {               "source_gap",               write(source.source_gap) },
        {       "initialization_gap",       write(source.initialization_gap) },
        {          "workspace_pools",          write(source.workspace_pools) },
        {    "workspace_binding_gap",    write(source.workspace_binding_gap) },
        {         "admission_policy",         write(source.admission_policy) },
        {                  "kernels",                  write(source.kernels) },
        {                "transfers",                write(source.transfers) },
        {              "model_rules",                 std::move(model_rules) },
        {          "workspace_rules",             std::move(workspace_rules) },
        {          "attention_rules",             std::move(attention_rules) },
        {         "collective_rules",            std::move(collective_rules) },
        {        "region_structures",                  std::move(structures) },
        {              "definitions",                 std::move(definitions) },
        {                     "gaps",                            source.gaps },
        {      "request_pool_domain",      write(source.request_pool_domain) },
        { "sequence_capacity_tokens", write(source.sequence_capacity_tokens) },
        {               "transforms",                      source.transforms },
        {                   "limits",                          source.limits }
    };
}

TemplateSource TemplateAssetCodec::decode(const Json & data) {
    TemplateSource result;
    result.manifest_path = data.at("manifest_path").get<std::string>();
    result.templates_path = data.at("templates_path").get<std::string>();
    result.model_path = data.at("model_path").get<std::string>();
    result.configuration = data.at("configuration");
    result.ranks = read<std::uint32_t>(data.at("ranks"));
    result.page_size_tokens = read<std::uint32_t>(data.at("page_size_tokens"));
    result.deployment = read<Environment>(data.at("deployment"));
    result.resolution = read<std::string>(data.at("resolution"));
    result.source_gap = read<std::optional<TemplateGap>>(data.at("source_gap"));
    result.initialization_gap = read<std::optional<std::string>>(data.at("initialization_gap"));
    result.admission_policy = read<std::expected<AdmissionPolicy, TemplateGap>>(data.at("admission_policy"));

    // Owners must be stable before restoring the indices embedded in definitions.
    result.kernels = read<std::map<SourceSpanId, SourceKernel>>(data.at("kernels"));
    for (const auto & [span, kernel] : result.kernels) kernels_.add(kernel);
    result.transfers = read<std::map<SourceSpanId, SourceTransfer>>(data.at("transfers"));
    for (const auto & [span, transfer] : result.transfers) transfers_.add(transfer);
    for (const auto & row : data.at("model_rules")) {
        const auto [entry, inserted] = result.model_rules.insert(read<ModelParameterRule>(row));
        model_rules_.add(*entry);
    }
    for (const auto & row : data.at("collective_rules")) {
        const auto [entry, inserted] = result.collective_rules.insert(read<CollectiveParameterRule>(row));
        collective_rules_.add(*entry);
    }
    for (const auto & row : data.at("workspace_rules")) {
        const auto [entry, inserted] = result.workspace_rules.insert(read<WorkspaceDemandLayout>(row));
        workspace_rules_.add(*entry);
    }
    for (const auto & row : data.at("attention_rules")) {
        const auto [entry, inserted] = result.attention_rules.insert(read<AttentionParameterRule>(row));
        attention_rules_.add(*entry);
    }
    for (const auto & row : data.at("region_structures")) {
        const auto [entry, inserted] = result.region_structures.insert(read<TemplateRegionStructure>(row));
        structures_.add(*entry);
    }
    for (const auto & row : data.at("definitions")) {
        auto value = definition(row);
        result.definitions[{ value.step, value.phase, value.rank }].push_back(std::move(value));
    }

    result.workspace_pools = read<std::vector<TemplateWorkspacePool>>(data.at("workspace_pools"));
    result.workspace_binding_gap = read<std::optional<std::string>>(data.at("workspace_binding_gap"));
    result.gaps = data.at("gaps");
    result.request_pool_domain = read<RequestPoolCapacityDomain>(data.at("request_pool_domain"));
    result.sequence_capacity_tokens = read<std::optional<std::uint32_t>>(data.at("sequence_capacity_tokens"));
    result.transforms = read<std::vector<std::string>>(data.at("transforms"));
    result.limits = read<std::vector<std::string>>(data.at("limits"));
    return result;
}

void TemplateAsset::write(const std::filesystem::path & path, const TemplateSource & source) {
    const auto data = TemplateAssetCodec{}.encode(source);
    auto pending = path;
    pending += ".tmp";
    std::ofstream stream;
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream.open(pending, std::ios::binary);
    Json::to_cbor(data, stream);
    stream.close();
    std::filesystem::rename(pending, path);
}

TemplateSource TemplateAsset::read(const std::filesystem::path & path) {
    std::ifstream stream;
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream.open(path, std::ios::binary);
    return TemplateAssetCodec{}.decode(Json::from_cbor(stream));
}

} // namespace markov
