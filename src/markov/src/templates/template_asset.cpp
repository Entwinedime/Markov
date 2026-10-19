#include "markov/templates/template_asset.hpp"

#include "markov/contracts/json_input.hpp"
#include "markov/templates/templates.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <limits>
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
// record declares its fields once; ownership restoration remains explicit.
class TemplateAssetCodec {
public:
    Json encode(const TemplateSource & source);
    TemplateSource decode(const Json & data);

private:
    template <class T> Json write(const T & value) const {
        if constexpr (requires { members(std::type_identity<T>{}); }) {
            return std::apply([&](auto... field) { return record(value.*field...); }, members(std::type_identity<T>{}));
        }
        else return Json(value);
    }
    template <class T> T read(const Json & value) const { return read(value, std::type_identity<T>{}); }
    template <class T> T read(const Json & data, std::type_identity<T> type) const {
        if constexpr (requires { members(type); }) {
            // Field indices and types come from the same declaration used by writing.
            return [&]<std::size_t... I>(std::index_sequence<I...>) {
                return T{ read<std::remove_cvref_t<decltype(std::declval<T>().*std::get<I>(members(type)))>>(data.at(I))... };
            }(std::make_index_sequence<std::tuple_size_v<decltype(members(type))>>{});
        }
        else return data.get<T>();
    }

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
    Json write(const AttentionParameterRule * value) const { return attention_rules_.encode(value); }
    const AttentionParameterRule * read(const Json & data, std::type_identity<const AttentionParameterRule *>) const { return attention_rules_.decode(data); }
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

    static constexpr auto members(std::type_identity<Environment>) { return std::tuple{ &Environment::service, &Environment::devices }; }

    static constexpr auto members(std::type_identity<AdmissionPolicy>) {
        return std::tuple{ &AdmissionPolicy::initial_ratio,
                           &AdmissionPolicy::minimum_ratio,
                           &AdmissionPolicy::decay_per_decode,
                           &AdmissionPolicy::output_estimate_limit_tokens,
                           &AdmissionPolicy::reserve_tokens_per_request };
    }

    static constexpr auto members(std::type_identity<BatchTransitionState>) {
        return std::tuple{ &BatchTransitionState::running_requests, &BatchTransitionState::pending_prefill_requests, &BatchTransitionState::admission_blocked };
    }

    static constexpr auto members(std::type_identity<AdmissionEntryState>) {
        return std::tuple{ &AdmissionEntryState::waiting_requests, &AdmissionEntryState::running_requests, &AdmissionEntryState::admission_blocked };
    }

    static constexpr auto members(std::type_identity<AdmissionWork>) {
        return std::tuple{ &AdmissionWork::candidate_checks, &AdmissionWork::request_horizon_checks, &AdmissionWork::stop };
    }

    static constexpr auto members(std::type_identity<BatchFilter>) { return std::tuple{ &BatchFilter::input_count, &BatchFilter::retained_indices }; }

    static constexpr auto members(std::type_identity<BatchMerge>) { return std::tuple{ &BatchMerge::left_count, &BatchMerge::right_count }; }

    static constexpr auto members(std::type_identity<SourceLane>) { return std::tuple{ &SourceLane::domain, &SourceLane::rank, &SourceLane::native_id }; }

    static constexpr auto members(std::type_identity<SourceTransfer>) {
        return std::tuple{ &SourceTransfer::bytes, &SourceTransfer::direction, &SourceTransfer::source_api };
    }

    static constexpr auto members(std::type_identity<KvSequence>) {
        return std::tuple{ &KvSequence::input_tokens, &KvSequence::prefix_tokens, &KvSequence::request_pool_slot, &KvSequence::kv_pages };
    }

    static constexpr auto members(std::type_identity<AttentionWorkload>) {
        return std::tuple{ &AttentionWorkload::operation, &AttentionWorkload::sequences,        &AttentionWorkload::query_heads, &AttentionWorkload::kv_heads,
                           &AttentionWorkload::head_dim,  &AttentionWorkload::page_size_tokens, &AttentionWorkload::cache_pages };
    }

    static constexpr auto members(std::type_identity<PositionSequence>) {
        return std::tuple{ &PositionSequence::prefix_tokens, &PositionSequence::input_tokens };
    }

    static constexpr auto members(std::type_identity<PositionWorkload>) { return std::tuple{ &PositionWorkload::sequences }; }

    Json write(const RequestPoolWork & value) const { return value.describe(); }
    RequestPoolWork read(const Json & data, std::type_identity<RequestPoolWork>) const { return RequestPoolWork::read(data); }

    static constexpr auto members(std::type_identity<SourceKernel>) {
        return std::tuple{ &SourceKernel::name,          &SourceKernel::operation,         &SourceKernel::input_shapes,
                           &SourceKernel::output_shapes, &SourceKernel::input_dtypes,      &SourceKernel::output_dtypes,
                           &SourceKernel::input_formats, &SourceKernel::output_formats,    &SourceKernel::logits_token_indices,
                           &SourceKernel::positions,     &SourceKernel::batch_filter,      &SourceKernel::request_pool,
                           &SourceKernel::attention,     &SourceKernel::value_dependencies };
    }

    static constexpr auto members(std::type_identity<SourceCommunicationTask>) {
        return std::tuple{ &SourceCommunicationTask::span,        &SourceCommunicationTask::source_detail,   &SourceCommunicationTask::stream,
                           &SourceCommunicationTask::task,        &SourceCommunicationTask::context,         &SourceCommunicationTask::operation,
                           &SourceCommunicationTask::bytes,       &SourceCommunicationTask::data_type,       &SourceCommunicationTask::transport,
                           &SourceCommunicationTask::source_rank, &SourceCommunicationTask::destination_rank };
    }

    static constexpr auto members(std::type_identity<SourceRequest>) {
        return std::tuple{ &SourceRequest::id,
                           &SourceRequest::input_tokens,
                           &SourceRequest::context_tokens,
                           &SourceRequest::prompt_tokens,
                           &SourceRequest::generated_tokens };
    }


    static constexpr auto members(std::type_identity<TensorParameterAxis>) {
        return std::tuple{ &TensorParameterAxis::slot, &TensorParameterAxis::multiplier, &TensorParameterAxis::extent, &TensorParameterAxis::axis };
    }

    static constexpr auto members(std::type_identity<CommunicationCountDomain>) {
        return std::tuple{ &CommunicationCountDomain::minimum, &CommunicationCountDomain::maximum, &CommunicationCountDomain::gap };
    }

    static constexpr auto members(std::type_identity<CommunicationTaskBytes>) {
        return std::tuple{ &CommunicationTaskBytes::task, &CommunicationTaskBytes::extent };
    }

    static constexpr auto members(std::type_identity<CollectiveCountRule>) { return std::tuple{ &CollectiveCountRule::rows, &CollectiveCountRule::width }; }

    static constexpr auto members(std::type_identity<CollectiveParameterRule>) {
        return std::tuple{ &CollectiveParameterRule::count, &CollectiveParameterRule::work };
    }

    static constexpr auto members(std::type_identity<BatchOperationVariant>) {
        return std::tuple{ &BatchOperationVariant::role,
                           &BatchOperationVariant::source_requests,
                           &BatchOperationVariant::minimum_requests,
                           &BatchOperationVariant::maximum_requests,
                           &BatchOperationVariant::page_table,
                           &BatchOperationVariant::page_size_tokens,
                           &BatchOperationVariant::request_pool_rebindable };
    }

    static constexpr auto members(std::type_identity<BatchRowParameter>) {
        return std::tuple{ &BatchRowParameter::output, &BatchRowParameter::slot, &BatchRowParameter::extent };
    }

    static constexpr auto members(std::type_identity<BatchRequestBinding>) {
        return std::tuple{ &BatchRequestBinding::request_count, &BatchRequestBinding::slot };
    }

    static constexpr auto members(std::type_identity<KvParameterAxis>) {
        return std::tuple{ &KvParameterAxis::output, &KvParameterAxis::slot, &KvParameterAxis::extent };
    }

    static constexpr auto members(std::type_identity<KvParameterDomain>) {
        return std::tuple{ &KvParameterDomain::maximum_new_pages,
                           &KvParameterDomain::allocates_pages,
                           &KvParameterDomain::release_free_empty,
                           &KvParameterDomain::release_request_slot };
    }

    static constexpr auto members(std::type_identity<RequestPoolCapacityDomain>) {
        return std::tuple{ &RequestPoolCapacityDomain::maximum, &RequestPoolCapacityDomain::shrink_gap };
    }

    static constexpr auto members(std::type_identity<TransferParameterLayout>) {
        return std::tuple{ &TransferParameterLayout::extent,
                           &TransferParameterLayout::request_count,
                           &TransferParameterLayout::finished_count,
                           &TransferParameterLayout::element_bytes };
    }

    static constexpr auto members(std::type_identity<TemplateResource>) {
        return std::tuple{ &TemplateResource::domain, &TemplateResource::rank, &TemplateResource::native_id, &TemplateResource::role };
    }

    static constexpr auto members(std::type_identity<TemplateLane>) {
        return std::tuple{ &TemplateLane::source_lane, &TemplateLane::source, &TemplateLane::role };
    }

    static constexpr auto members(std::type_identity<TemplateBoundary>) { return std::tuple{ &TemplateBoundary::lane }; }

    static constexpr auto members(std::type_identity<TemplateBoundaryObservation>) {
        return std::tuple{ &TemplateBoundaryObservation::source_boundary, &TemplateBoundaryObservation::observed_time_ns };
    }

    static constexpr auto members(std::type_identity<CpuBoundary>) {
        return std::tuple{ &CpuBoundary::role, &CpuBoundary::kind, &CpuBoundary::edge, &CpuBoundary::name, &CpuBoundary::ordinal, &CpuBoundary::count };
    }

    static constexpr auto members(std::type_identity<WorkspacePartition>) {
        return std::tuple{ &WorkspacePartition::groups,
                           &WorkspacePartition::group_size_limit,
                           &WorkspacePartition::minimum_members,
                           &WorkspacePartition::maximum_sequence_exclusive,
                           &WorkspacePartition::group_size_by_members };
    }
    static constexpr auto members(std::type_identity<WorkspaceScaledBuffer>) {
        return std::tuple{ &WorkspaceScaledBuffer::scale, &WorkspaceScaledBuffer::groups, &WorkspaceScaledBuffer::element_bytes };
    }
    static constexpr auto members(std::type_identity<WorkspaceDemandLayout>) {
        return std::tuple{ &WorkspaceDemandLayout::units_,
                           &WorkspaceDemandLayout::bytes_per_unit_,
                           &WorkspaceDemandLayout::alignment_,
                           &WorkspaceDemandLayout::partition_,
                           &WorkspaceDemandLayout::buffers_ };
    }

    static constexpr auto members(std::type_identity<TemplateWorkspacePool>) {
        return std::tuple{ &TemplateWorkspacePool::resource,
                           &TemplateWorkspacePool::initial_capacity_bytes,
                           &TemplateWorkspacePool::allocation_padding_bytes,
                           &TemplateWorkspacePool::allocation_alignment_bytes };
    }
    static constexpr auto members(std::type_identity<TemplateWorkspaceUse>) {
        return std::tuple{ &TemplateWorkspaceUse::pool,
                           &TemplateWorkspaceUse::operation,
                           &TemplateWorkspaceUse::worker,
                           &TemplateWorkspaceUse::source_api,
                           &TemplateWorkspaceUse::marker,
                           &TemplateWorkspaceUse::begin,
                           &TemplateWorkspaceUse::end,
                           &TemplateWorkspaceUse::requested_bytes,
                           &TemplateWorkspaceUse::capacity_before_bytes,
                           &TemplateWorkspaceUse::capacity_after_bytes,
                           &TemplateWorkspaceUse::branch,
                           &TemplateWorkspaceUse::requires_device_synchronization,
                           &TemplateWorkspaceUse::demand };
    }

    static constexpr auto members(std::type_identity<TemplateStreamWait>) {
        return std::tuple{ &TemplateStreamWait::source_span,
                           &TemplateStreamWait::native_stream,
                           &TemplateStreamWait::completion,
                           &TemplateStreamWait::resources,
                           &TemplateStreamWait::scope };
    }

    static constexpr auto members(std::type_identity<TemplateNode>) {
        return std::tuple{ &TemplateNode::observation_slot, &TemplateNode::kind, &TemplateNode::begin, &TemplateNode::end };
    }

    static constexpr auto members(std::type_identity<TemplateDependency>) {
        return std::tuple{ &TemplateDependency::relation, &TemplateDependency::source, &TemplateDependency::target };
    }

    static constexpr auto members(std::type_identity<TemplateConnectionContract>) {
        return std::tuple{ &TemplateConnectionContract::relation, &TemplateConnectionContract::direction, &TemplateConnectionContract::local_boundary };
    }

    static constexpr auto members(std::type_identity<TemplateInterface>) {
        return std::tuple{ &TemplateInterface::connection, &TemplateInterface::remote_boundary };
    }

    static constexpr auto members(std::type_identity<TemplateResourceSlot>) {
        return std::tuple{ &TemplateResourceSlot::domain, &TemplateResourceSlot::rank, &TemplateResourceSlot::role };
    }

    static constexpr auto members(std::type_identity<TemplateRemoteBoundary>) {
        return std::tuple{ &TemplateRemoteBoundary::lane, &TemplateRemoteBoundary::local_boundary };
    }

    static constexpr auto members(std::type_identity<TemplateRegionStructure>) {
        return std::tuple{ &TemplateRegionStructure::resources,         &TemplateRegionStructure::boundaries,       &TemplateRegionStructure::nodes,
                           &TemplateRegionStructure::dependencies,      &TemplateRegionStructure::connections,      &TemplateRegionStructure::dependency_ports,
                           &TemplateRegionStructure::remote_boundaries, &TemplateRegionStructure::collective_rules, &TemplateRegionStructure::model_rules,
                           &TemplateRegionStructure::semantic };
    }

    static constexpr auto members(std::type_identity<TemplateConnectionObservation>) {
        return std::tuple{ &TemplateConnectionObservation::source_edge,
                           &TemplateConnectionObservation::remote_source_calls,
                           &TemplateConnectionObservation::resource,
                           &TemplateConnectionObservation::remote_order };
    }

    static constexpr auto members(std::type_identity<TemplateRegionScope>) { return std::tuple{ &TemplateRegionScope::begin, &TemplateRegionScope::end }; }

    static constexpr auto members(std::type_identity<TemplateRegion>) {
        return std::tuple{ &TemplateRegion::source_call, &TemplateRegion::parent_call, &TemplateRegion::semantic,     &TemplateRegion::nodes,
                           &TemplateRegion::inputs,      &TemplateRegion::outputs,     &TemplateRegion::request_slot, &TemplateRegion::cpu_scope };
    }

    static constexpr auto members(std::type_identity<RotaryPositionDependency>) {
        return std::tuple{ &RotaryPositionDependency::producer, &RotaryPositionDependency::consumer };
    }

    static constexpr auto members(std::type_identity<TemplateOperationWindow>) {
        return std::tuple{ &TemplateOperationWindow::source_span, &TemplateOperationWindow::begin, &TemplateOperationWindow::end };
    }

    static constexpr auto members(std::type_identity<TemplateOperation>) {
        return std::tuple{ &TemplateOperation::name,
                           &TemplateOperation::host,
                           &TemplateOperation::workers,
                           &TemplateOperation::device_spans,
                           &TemplateOperation::collective_spans };
    }

    static constexpr auto members(std::type_identity<TemplateCollective>) {
        return std::tuple{ &TemplateCollective::source_span,   &TemplateCollective::data_type,  &TemplateCollective::tasks,
                           &TemplateCollective::envelope_node, &TemplateCollective::task_nodes, &TemplateCollective::serial_completion };
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

    static constexpr auto members(std::type_identity<KvRegionBranch>) {
        return std::tuple{ &KvRegionBranch::kind,
                           &KvRegionBranch::region,
                           &KvRegionBranch::changes_pages,
                           &KvRegionBranch::minimum_members,
                           &KvRegionBranch::maximum_members };
    }

    static constexpr auto members(std::type_identity<PeriodicOutputPolicy>) { return std::tuple{ &PeriodicOutputPolicy::interval_tokens }; }

    static constexpr auto members(std::type_identity<TemplateGap>) {
        return std::tuple{ &TemplateGap::kind, &TemplateGap::reason, &TemplateGap::step, &TemplateGap::phase, &TemplateGap::rank };
    }

    static constexpr auto members(std::type_identity<TensorParameterLayout>) {
        return std::tuple{ &TensorParameterLayout::inputs_, &TensorParameterLayout::outputs_, &TensorParameterLayout::page_size_tokens_ };
    }

    Json write(const ModelParameterRule & value) const { return record(value.shapes_, value.workload_); }
    ModelParameterRule read(const Json & data, std::type_identity<ModelParameterRule>) const {
        auto shapes = read<TensorParameterLayout>(data.at(0));
        return { std::move(shapes.inputs_), std::move(shapes.outputs_), shapes.page_size_tokens_, read<ModelWorkload>(data.at(1)) };
    }

    static constexpr auto members(std::type_identity<CommunicationWorkLayout>) {
        return std::tuple{ &CommunicationWorkLayout::operation_, &CommunicationWorkLayout::element_bytes_, &CommunicationWorkLayout::partitions_,
                           &CommunicationWorkLayout::alignment_, &CommunicationWorkLayout::domains_,       &CommunicationWorkLayout::relations_ };
    }

    static constexpr auto members(std::type_identity<AttentionParameterRule>) {
        return std::tuple{ &AttentionParameterRule::shapes_, &AttentionParameterRule::workload_ };
    }

    static constexpr auto members(std::type_identity<BatchParameterLayout>) {
        return std::tuple{ &BatchParameterLayout::filter_, &BatchParameterLayout::rows_, &BatchParameterLayout::request_, &BatchParameterLayout::input_arity_ };
    }

    static constexpr auto members(std::type_identity<KvParameterLayout>) {
        return std::tuple{ &KvParameterLayout::kind_, &KvParameterLayout::page_size_, &KvParameterLayout::domain_,
                           &KvParameterLayout::axes_, &KvParameterLayout::output_,    &KvParameterLayout::release_member_inputs_ };
    }

    static constexpr auto members(std::type_identity<RequestPoolParameterLayout>) {
        return std::tuple{ &RequestPoolParameterLayout::shapes_, &RequestPoolParameterLayout::access_, &RequestPoolParameterLayout::single_request_only_ };
    }

    static constexpr auto members(std::type_identity<PublicationParameterLayout>) {
        return std::tuple{ &PublicationParameterLayout::source_span_,
                           &PublicationParameterLayout::interval_tokens_,
                           &PublicationParameterLayout::context_limit_,
                           &PublicationParameterLayout::sender_enabled_ };
    }

    static constexpr auto members(std::type_identity<TemplateGraph::Location>) {
        return std::tuple{ &TemplateGraph::Location::region, &TemplateGraph::Location::member };
    }

    static constexpr auto members(std::type_identity<TemplateGraph::RemoteBoundaryBinding>) {
        return std::tuple{ &TemplateGraph::RemoteBoundaryBinding::source_boundary, &TemplateGraph::RemoteBoundaryBinding::local_boundary };
    }

    static constexpr auto members(std::type_identity<TemplateGraph::ObservedConnection>) {
        return std::tuple{ &TemplateGraph::ObservedConnection::contract,
                           &TemplateGraph::ObservedConnection::remote_source_boundary,
                           &TemplateGraph::ObservedConnection::remote_lane };
    }

    static constexpr auto members(std::type_identity<TemplateGraph::Binding>) {
        return std::tuple{ &TemplateGraph::Binding::structure,
                           &TemplateGraph::Binding::boundaries,
                           &TemplateGraph::Binding::remote_boundaries,
                           &TemplateGraph::Binding::lanes,
                           &TemplateGraph::Binding::source_region };
    }

    Json write(const DeviceCommandWork & value) const { return value.describe(); }
    DeviceCommandWork read(const Json & data, std::type_identity<DeviceCommandWork>) const { return DeviceCommandWork::read(data); }

    static constexpr auto members(std::type_identity<TemplateNodeObservation>) {
        return std::tuple{ &TemplateNodeObservation::source_span,   &TemplateNodeObservation::kernel,   &TemplateNodeObservation::source_scope,
                           &TemplateNodeObservation::communication, &TemplateNodeObservation::transfer, &TemplateNodeObservation::device_command };
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
            for (const auto & task : collective.tasks) tasks_.add(task);

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
            {                 "filter_regions",                 write(value.filter_regions) },
            {                          "merge",                          write(value.merge) },
            {                          "entry",                          write(value.entry) },
            {                          "ports",                          write(value.ports) },
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
            {                 "cpu_boundaries",                 write(value.cpu_boundaries) },
            {                          "graph",                          write(value.graph) },
            {              "node_observations",              write(value.node_observations) },
            {                        "regions",                        write(value.regions) },
            {                      "kv_region",                      write(value.kv_region) },
            {                     "operations",                     write(value.operations) },
            {                 "workspace_uses",                 write(value.workspace_uses) },
            {               "batch_operations",               write(value.batch_operations) },
            {               "transfer_layouts",               write(value.transfer_layouts) },
            {                    "collectives",                    write(value.collectives) },
            {           "model_kernel_layouts",           write(value.model_kernel_layouts) },
            {       "attention_kernel_layouts",       write(value.attention_kernel_layouts) },
            {              "kv_kernel_layouts",              write(value.kv_kernel_layouts) },
            {           "request_pool_layouts",           write(value.request_pool_layouts) },
            {           "batch_kernel_layouts",           write(value.batch_kernel_layouts) },
            {                   "stream_waits",                   write(value.stream_waits) },
            {   "rotary_position_dependencies",   write(value.rotary_position_dependencies) },
            {           "single_request_views",           write(value.single_request_views) },
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
            for (const auto & task : collective.tasks) tasks_.add(task);

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
        result.filter_regions = read<std::map<BatchFilterBinding, std::size_t>>(data.at("filter_regions"));
        result.merge = read<std::optional<BatchMerge>>(data.at("merge"));

        result.entry = read<std::size_t>(data.at("entry"));
        result.ports = read<std::map<PortKind, std::size_t>>(data.at("ports"));
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
        result.cpu_boundaries = read<std::map<std::size_t, CpuBoundary>>(data.at("cpu_boundaries"));
        result.graph = read<TemplateGraph>(data.at("graph"));
        result.node_observations = read<std::vector<TemplateNodeObservation>>(data.at("node_observations"));

        result.regions = read<std::vector<TemplateRegion>>(data.at("regions"));
        result.kv_region = read<std::optional<KvRegionBranch>>(data.at("kv_region"));
        result.operations = read<std::vector<TemplateOperation>>(data.at("operations"));
        result.workspace_uses = read<std::vector<TemplateWorkspaceUse>>(data.at("workspace_uses"));
        result.batch_operations = read<std::map<SourceSpanId, BatchOperationVariant>>(data.at("batch_operations"));
        result.transfer_layouts = read<std::map<SourceSpanId, TransferParameterLayout>>(data.at("transfer_layouts"));

        result.model_kernel_layouts = read<std::map<SourceSpanId, std::size_t>>(data.at("model_kernel_layouts"));
        result.attention_kernel_layouts = read<std::map<SourceSpanId, const AttentionParameterRule *>>(data.at("attention_kernel_layouts"));
        result.kv_kernel_layouts = read<std::map<SourceSpanId, std::expected<KvParameterLayout, std::string>>>(data.at("kv_kernel_layouts"));
        result.request_pool_layouts = read<std::map<SourceSpanId, std::expected<RequestPoolParameterLayout, std::string>>>(data.at("request_pool_layouts"));
        result.batch_kernel_layouts = read<std::map<SourceSpanId, std::expected<BatchParameterLayout, std::string>>>(data.at("batch_kernel_layouts"));

        result.stream_waits = read<std::vector<TemplateStreamWait>>(data.at("stream_waits"));
        result.rotary_position_dependencies = read<std::vector<RotaryPositionDependency>>(data.at("rotary_position_dependencies"));
        result.single_request_views = read<std::map<std::size_t, std::optional<std::size_t>>>(data.at("single_request_views"));
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
        {              "manifest_path",            source.manifest_path.string() },
        {                 "model_path",                        source.model_path },
        {              "configuration",                     source.configuration },
        {                      "ranks",                             source.ranks },
        {           "page_size_tokens",                  source.page_size_tokens },
        {                 "deployment",                 write(source.deployment) },
        {                 "resolution",                        source.resolution },
        {                 "source_gap",                 write(source.source_gap) },
        {           "steady_state_gap",           write(source.steady_state_gap) },
        {            "workspace_pools",            write(source.workspace_pools) },
        {      "workspace_binding_gap",      write(source.workspace_binding_gap) },
        {           "admission_policy",           write(source.admission_policy) },
        {                    "kernels",                    write(source.kernels) },
        {                  "transfers",                  write(source.transfers) },
        {                "model_rules",                   std::move(model_rules) },
        {            "workspace_rules",               std::move(workspace_rules) },
        {            "attention_rules",               std::move(attention_rules) },
        {           "collective_rules",              std::move(collective_rules) },
        {          "region_structures",                    std::move(structures) },
        {                "definitions",                   std::move(definitions) },
        {                       "gaps",                              source.gaps },
        {        "request_pool_domain",        write(source.request_pool_domain) },
        { "initialization_pool_domain", write(source.initialization_pool_domain) },
        {   "sequence_capacity_tokens",   write(source.sequence_capacity_tokens) },
        {                 "transforms",                        source.transforms },
        {                     "limits",                            source.limits }
    };
}

TemplateSource TemplateAssetCodec::decode(const Json & data) {
    TemplateSource result;
    result.manifest_path = data.at("manifest_path").get<std::string>();
    result.model_path = data.at("model_path").get<std::string>();
    result.configuration = data.at("configuration");
    result.ranks = read<std::uint32_t>(data.at("ranks"));
    result.page_size_tokens = read<std::uint32_t>(data.at("page_size_tokens"));
    result.deployment = read<Environment>(data.at("deployment"));
    result.resolution = read<std::string>(data.at("resolution"));
    result.source_gap = read<std::optional<TemplateGap>>(data.at("source_gap"));
    result.steady_state_gap = read<std::optional<std::string>>(data.at("steady_state_gap"));
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
    result.initialization_pool_domain = read<RequestPoolCapacityDomain>(data.at("initialization_pool_domain"));
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
