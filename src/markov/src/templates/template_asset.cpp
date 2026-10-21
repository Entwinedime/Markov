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

// Each direction builds only its lookup: address indices for encoding, indexed
// owners for decoding. Decoded references borrow already restored owners.
template <class T> class AssetReferences {
public:
    void index(const T & value) { indices_.emplace(&value, indices_.size()); }
    void restore(const T & value) { values_.push_back(&value); }
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
    Json write(const TemplateRegionStructure * value) const { return structures_.encode(value); }
    const TemplateRegionStructure * read(const Json & data, std::type_identity<const TemplateRegionStructure *>) const { return structures_.decode(data); }
    Json write(const SourceKernel * value) const { return kernels_.encode(value); }
    const SourceKernel * read(const Json & data, std::type_identity<const SourceKernel *>) const { return kernels_.decode(data); }
    Json write(const SourceTransfer * value) const { return transfers_.encode(value); }
    const SourceTransfer * read(const Json & data, std::type_identity<const SourceTransfer *>) const { return transfers_.decode(data); }

    static constexpr auto members(std::type_identity<Environment>) {
        return std::tuple{ &Environment::service, &Environment::devices, &Environment::image_id, &Environment::measurement };
    }

    static constexpr auto members(std::type_identity<TemplateRankBinding>) {
        return std::tuple{ &TemplateRankBinding::source_rank,
                           &TemplateRankBinding::completion_ports,
                           &TemplateRankBinding::publishes_outputs,
                           &TemplateRankBinding::receives_inputs };
    }

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

    static constexpr auto members(std::type_identity<SourceRequest>) {
        return std::tuple{ &SourceRequest::id,
                           &SourceRequest::input_tokens,
                           &SourceRequest::context_tokens,
                           &SourceRequest::prompt_tokens,
                           &SourceRequest::generated_tokens };
    }

    static constexpr auto members(std::type_identity<SourceIncomingRequest>) {
        return std::tuple{ &SourceIncomingRequest::id, &SourceIncomingRequest::prompt_tokens, &SourceIncomingRequest::output_tokens };
    }


    static constexpr auto members(std::type_identity<TensorParameterAxis>) {
        return std::tuple{ &TensorParameterAxis::slot, &TensorParameterAxis::multiplier, &TensorParameterAxis::extent, &TensorParameterAxis::axis };
    }

    static constexpr auto members(std::type_identity<CollectiveCountRule>) { return std::tuple{ &CollectiveCountRule::rows, &CollectiveCountRule::width }; }
    static constexpr auto members(std::type_identity<HostMessageRule>) {
        return std::tuple{ &HostMessageRule::root, &HostMessageRule::completion_entry_dependencies };
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
                           &TemplateWorkspaceUse::source_api,
                           &TemplateWorkspaceUse::marker,
                           &TemplateWorkspaceUse::begin,
                           &TemplateWorkspaceUse::end,
                           &TemplateWorkspaceUse::requested_bytes,
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

    static constexpr auto members(std::type_identity<TemplateConnectionEndpoint>) {
        return std::tuple{ &TemplateConnectionEndpoint::contract, &TemplateConnectionEndpoint::remote_source_boundary };
    }

    static constexpr auto members(std::type_identity<TemplateResourceSlot>) {
        return std::tuple{ &TemplateResourceSlot::domain, &TemplateResourceSlot::rank, &TemplateResourceSlot::role };
    }

    static constexpr auto members(std::type_identity<TemplateRegionStructure>) {
        return std::tuple{ &TemplateRegionStructure::resources,    &TemplateRegionStructure::boundaries,  &TemplateRegionStructure::nodes,
                           &TemplateRegionStructure::dependencies, &TemplateRegionStructure::model_rules, &TemplateRegionStructure::semantic };
    }

    static constexpr auto members(std::type_identity<TemplateConnectionObservation>) {
        return std::tuple{ &TemplateConnectionObservation::source_edge,
                           &TemplateConnectionObservation::remote_source_calls,
                           &TemplateConnectionObservation::resource,
                           &TemplateConnectionObservation::remote_order };
    }

    static constexpr auto members(std::type_identity<TemplateRegionScope>) { return std::tuple{ &TemplateRegionScope::begin, &TemplateRegionScope::end }; }

    static constexpr auto members(std::type_identity<TemplateRegionSequence>) {
        return std::tuple{ &TemplateRegionSequence::source_members,    &TemplateRegionSequence::members, &TemplateRegionSequence::parameter,
                           &TemplateRegionSequence::separators,        &TemplateRegionSequence::inputs,  &TemplateRegionSequence::gap,
                           &TemplateRegionSequence::repeated_separator };
    }
    static constexpr auto members(std::type_identity<TemplateSequenceInput>) {
        return std::tuple{ &TemplateSequenceInput::step,
                           &TemplateSequenceInput::dependency,
                           &TemplateSequenceInput::previous_resource,
                           &TemplateSequenceInput::producer_step };
    }
    static constexpr auto members(std::type_identity<TemplateDependencyOrigin>) {
        return std::tuple{ &TemplateDependencyOrigin::kind, &TemplateDependencyOrigin::slot };
    }

    static constexpr auto members(std::type_identity<TemplateCollectiveInterface>) {
        return std::tuple{ &TemplateCollectiveInterface::position,
                           &TemplateCollectiveInterface::operation,
                           &TemplateCollectiveInterface::count,
                           &TemplateCollectiveInterface::data_type,
                           &TemplateCollectiveInterface::group_size,
                           &TemplateCollectiveInterface::host,
                           &TemplateCollectiveInterface::owner_region,
                           &TemplateCollectiveInterface::lane,
                           &TemplateCollectiveInterface::inputs,
                           &TemplateCollectiveInterface::outputs,
                           &TemplateCollectiveInterface::model_region,
                           &TemplateCollectiveInterface::model_layer,
                           &TemplateCollectiveInterface::substitute_nodes,
                           &TemplateCollectiveInterface::transport_boundaries,
                           &TemplateCollectiveInterface::launch_inputs,
                           &TemplateCollectiveInterface::seam_dependencies,
                           &TemplateCollectiveInterface::host_message };
    }

    static constexpr auto members(std::type_identity<TemplateRegion>) {
        return std::tuple{ &TemplateRegion::semantic,     &TemplateRegion::inputs,    &TemplateRegion::outputs,
                           &TemplateRegion::request_slot, &TemplateRegion::cpu_scope, &TemplateRegion::input_slot };
    }
    static constexpr auto members(std::type_identity<TemplateRegionPort>) {
        return std::tuple{ &TemplateRegionPort::resource, &TemplateRegionPort::entry, &TemplateRegionPort::exit, &TemplateRegionPort::device_done };
    }
    static constexpr auto cut_fields() {
        return std::tuple{ &TemplateSubgraphRegion::boundaries,
                           &TemplateSubgraphRegion::crossing_nodes,
                           &TemplateSubgraphRegion::retained_dependencies,
                           &TemplateSubgraphRegion::internal_dependencies,
                           &TemplateSubgraphRegion::incoming_dependencies,
                           &TemplateSubgraphRegion::outgoing_dependencies,
                           &TemplateSubgraphRegion::external_connections,
                           &TemplateSubgraphRegion::incoming_rotary_dependencies,
                           &TemplateSubgraphRegion::outgoing_rotary_dependencies,
                           &TemplateSubgraphRegion::resource_ports,
                           &TemplateSubgraphRegion::replacement_gap };
    }
    static constexpr auto members(std::type_identity<TemplateSubgraphRegion>) {
        return std::tuple_cat(std::tuple{ &TemplateSubgraphRegion::nodes }, cut_fields());
    }
    Json write(const TemplateRegion & value) const {
        auto result = record(value.source_call, value.parent_call, value.nodes, nullptr);
        if (!value.boundaries.empty()) result[3] = std::apply([&](auto... field) { return record(value.*field...); }, cut_fields());
        std::apply([&](auto... field) { (result.push_back(write(value.*field)), ...); }, members(std::type_identity<TemplateRegion>{}));
        return result;
    }
    TemplateRegion read(const Json & data, std::type_identity<TemplateRegion>) const {
        TemplateRegion result(read<std::size_t>(data.at(0)), read<std::size_t>(data.at(1)));
        result.nodes = read<std::vector<std::size_t>>(data.at(2));
        std::size_t index = 0;
        if (!data.at(3).is_null())
            std::apply([&](auto... field) { ((result.*field = read<std::remove_cvref_t<decltype(result.*field)>>(data.at(3).at(index++))), ...); },
                       cut_fields());
        index = 4;
        std::apply([&](auto... field) { ((result.*field = read<std::remove_cvref_t<decltype(result.*field)>>(data.at(index++))), ...); },
                   members(std::type_identity<TemplateRegion>{}));
        return result;
    }

    static constexpr auto members(std::type_identity<RotaryPositionDependency>) {
        return std::tuple{ &RotaryPositionDependency::producer, &RotaryPositionDependency::consumer };
    }

    static constexpr auto members(std::type_identity<TemplateOperationWindow>) {
        return std::tuple{ &TemplateOperationWindow::source_span, &TemplateOperationWindow::begin, &TemplateOperationWindow::end };
    }

    static constexpr auto members(std::type_identity<TemplateOperation>) {
        return std::tuple{ &TemplateOperation::name, &TemplateOperation::host, &TemplateOperation::workers, &TemplateOperation::device_spans };
    }

    static constexpr auto members(std::type_identity<RequestLengthDomain>) {
        return std::tuple{ &RequestLengthDomain::minimum_input_tokens,
                           &RequestLengthDomain::maximum_input_tokens,
                           &RequestLengthDomain::minimum_context_tokens,
                           &RequestLengthDomain::maximum_context_tokens };
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

    static constexpr auto members(std::type_identity<TemplateGraph::Binding>) {
        return std::tuple{ &TemplateGraph::Binding::structure, &TemplateGraph::Binding::boundaries, &TemplateGraph::Binding::lanes };
    }

    Json write(const DeviceCommandWork & value) const { return value.describe(); }
    DeviceCommandWork read(const Json & data, std::type_identity<DeviceCommandWork>) const { return DeviceCommandWork::read(data); }

    static constexpr auto members(std::type_identity<TemplateNodeObservation>) {
        return std::tuple{ &TemplateNodeObservation::source_span,
                           &TemplateNodeObservation::kernel,
                           &TemplateNodeObservation::source_scope,
                           &TemplateNodeObservation::transfer,
                           &TemplateNodeObservation::device_command };
    }

    static constexpr auto members(std::type_identity<TemplateGraph>) {
        return std::tuple{ &TemplateGraph::local_boundaries_,
                           &TemplateGraph::boundary_locations_,
                           &TemplateGraph::local_,
                           &TemplateGraph::locations_,
                           &TemplateGraph::bindings_,
                           &TemplateGraph::local_dependencies_,
                           &TemplateGraph::dependency_locations_,
                           &TemplateGraph::connections_,
                           &TemplateGraph::model_rules_ };
    }
    TemplateGraph read(const Json & data, std::type_identity<TemplateGraph>) const {
        TemplateGraph result;
        std::size_t index = 0;
        std::apply([&](auto... field) { ((result.*field = read<std::remove_cvref_t<decltype(result.*field)>>(data.at(index++))), ...); },
                   members(std::type_identity<TemplateGraph>{}));
        return result;
    }

    // Declare each named field once for both directions. Source-owned records
    // and reference tables are restored before these borrowing fields.
    static constexpr auto definition_fields() {
        using T = TemplateDefinition;
        return std::tuple{
            std::pair{                    "source_batch",                    &T::source_batch },
            std::pair{         "source_invocation_order",                    &T::source_order },
            std::pair{                      "transition",                      &T::transition },
            std::pair{               "transition_region",               &T::transition_region },
            std::pair{  "transition_latch_parameterized",  &T::transition_latch_parameterized },
            std::pair{       "transition_cpu_parameters",       &T::transition_cpu_parameters },
            std::pair{     "transition_merge_parameters",     &T::transition_merge_parameters },
            std::pair{           "running_update_region",           &T::running_update_region },
            std::pair{                       "admission",                       &T::admission },
            std::pair{                  "admission_work",                  &T::admission_work },
            std::pair{     "admission_cpu_request_count",     &T::admission_cpu_request_count },
            std::pair{              "admission_sequence",              &T::admission_sequence },
            std::pair{                         "filters",                         &T::filters },
            std::pair{                  "filter_regions",                  &T::filter_regions },
            std::pair{                           "merge",                           &T::merge },
            std::pair{                           "entry",                           &T::entry },
            std::pair{                           "ports",                           &T::ports },
            std::pair{                 "source_requests",                 &T::source_requests },
            std::pair{        "source_incoming_requests",        &T::source_incoming_requests },
            std::pair{                 "input_reception",                 &T::input_reception },
            std::pair{          "incoming_request_ready",          &T::incoming_request_ready },
            std::pair{       "incoming_request_sequence",       &T::incoming_request_sequence },
            std::pair{        "input_reception_sequence",        &T::input_reception_sequence },
            std::pair{                   "length_domain",                   &T::length_domain },
            std::pair{            "publication_finishes",            &T::publication_finishes },
            std::pair{                   "output_policy",                   &T::output_policy },
            std::pair{                 "output_boundary",                 &T::output_boundary },
            std::pair{              "publication_layout",              &T::publication_layout },
            std::pair{            "publication_sequence",            &T::publication_sequence },
            std::pair{ "request_initialization_sequence", &T::request_initialization_sequence },
            std::pair{        "publication_cpu_progress",        &T::publication_cpu_progress },
            std::pair{                           "lanes",                           &T::lanes },
            std::pair{           "boundary_observations",           &T::boundary_observations },
            std::pair{                  "cpu_boundaries",                  &T::cpu_boundaries },
            std::pair{                           "graph",                           &T::graph },
            std::pair{               "node_observations",               &T::node_observations },
            std::pair{                         "regions",                         &T::regions },
            std::pair{                       "sequences",                       &T::sequences },
            std::pair{                  "model_sequence",                  &T::model_sequence },
            std::pair{                       "kv_region",                       &T::kv_region },
            std::pair{                      "operations",                      &T::operations },
            std::pair{                  "workspace_uses",                  &T::workspace_uses },
            std::pair{                "batch_operations",                &T::batch_operations },
            std::pair{                "transfer_layouts",                &T::transfer_layouts },
            std::pair{           "collective_interfaces",           &T::collective_interfaces },
            std::pair{        "attention_kernel_layouts",        &T::attention_kernel_layouts },
            std::pair{               "kv_kernel_layouts",               &T::kv_kernel_layouts },
            std::pair{            "request_pool_layouts",            &T::request_pool_layouts },
            std::pair{            "batch_kernel_layouts",            &T::batch_kernel_layouts },
            std::pair{                    "stream_waits",                    &T::stream_waits },
            std::pair{    "rotary_position_dependencies",    &T::rotary_position_dependencies },
            std::pair{            "single_request_views",            &T::single_request_views },
            std::pair{         "connection_observations",         &T::connection_observations }
        };
    }

    Json write(const TemplateDefinition & value) {
        Json result{
            {        "step",        write(value.step) },
            {       "phase",       write(value.phase) },
            {        "rank",        write(value.rank) },
            { "source_call", write(value.source_call) }
        };
        std::apply([&](auto... field) { ((result[field.first] = write(value.*field.second)), ...); }, definition_fields());
        return result;
    }
    TemplateDefinition definition(const Json & data) {
        TemplateDefinition result(read<StepKind>(data.at("step")),
                                  read<BatchPhase>(data.at("phase")),
                                  read<std::uint32_t>(data.at("rank")),
                                  read<std::size_t>(data.at("source_call")));
        std::apply([&](auto... field) { ((result.*field.second = read<std::remove_cvref_t<decltype(result.*field.second)>>(data.at(field.first))), ...); },
                   definition_fields());
        return result;
    }

    AssetReferences<ModelParameterRule> model_rules_;
    AssetReferences<WorkspaceDemandLayout> workspace_rules_;
    AssetReferences<AttentionParameterRule> attention_rules_;
    AssetReferences<TemplateRegionStructure> structures_;
    AssetReferences<SourceKernel> kernels_;
    AssetReferences<SourceTransfer> transfers_;
};

Json TemplateAssetCodec::encode(const TemplateSource & source) {
    for (const auto & [span, kernel] : source.kernels) kernels_.index(kernel);
    for (const auto & [span, transfer] : source.transfers) transfers_.index(transfer);

    auto model_rules = Json::array();
    for (const auto & rule : source.model_rules) {
        model_rules_.index(rule);
        model_rules.push_back(write(rule));
    }
    auto workspace_rules = Json::array();
    for (const auto & rule : source.workspace_rules) {
        workspace_rules_.index(rule);
        workspace_rules.push_back(write(rule));
    }
    auto attention_rules = Json::array();
    for (const auto & rule : source.attention_rules) {
        attention_rules_.index(rule);
        attention_rules.push_back(write(rule));
    }
    auto structures = Json::array();
    for (const auto & structure : source.region_structures) {
        structures_.index(structure);
        structures.push_back(write(structure));
    }

    auto definitions = Json::array();
    for (const auto & [key, members] : source.definitions)
        for (const auto & member : members) definitions.push_back(write(member));

    return {
        {              "manifest_path",            source.manifest_path.string() },
        {        "model_configuration",               source.model_configuration },
        {              "configuration",                     source.configuration },
        {                "source_rank",                       source.source_rank },
        {              "rank_bindings",              write(source.rank_bindings) },
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
    result.model_configuration = data.at("model_configuration");
    result.configuration = data.at("configuration");
    result.source_rank = read<std::uint32_t>(data.at("source_rank"));
    result.rank_bindings = read<std::vector<TemplateRankBinding>>(data.at("rank_bindings"));
    result.page_size_tokens = read<std::uint32_t>(data.at("page_size_tokens"));
    result.deployment = read<Environment>(data.at("deployment"));
    result.resolution = read<std::string>(data.at("resolution"));
    result.source_gap = read<std::optional<TemplateGap>>(data.at("source_gap"));
    result.steady_state_gap = read<std::optional<std::string>>(data.at("steady_state_gap"));
    result.admission_policy = read<std::expected<AdmissionPolicy, TemplateGap>>(data.at("admission_policy"));

    // Owners must be stable before restoring the indices embedded in definitions.
    result.kernels = read<std::map<SourceSpanId, SourceKernel>>(data.at("kernels"));
    for (const auto & [span, kernel] : result.kernels) kernels_.restore(kernel);
    result.transfers = read<std::map<SourceSpanId, SourceTransfer>>(data.at("transfers"));
    for (const auto & [span, transfer] : result.transfers) transfers_.restore(transfer);
    for (const auto & row : data.at("model_rules")) {
        const auto [entry, inserted] = result.model_rules.insert(read<ModelParameterRule>(row));
        model_rules_.restore(*entry);
    }
    for (const auto & row : data.at("workspace_rules")) {
        const auto [entry, inserted] = result.workspace_rules.insert(read<WorkspaceDemandLayout>(row));
        workspace_rules_.restore(*entry);
    }
    for (const auto & row : data.at("attention_rules")) {
        const auto [entry, inserted] = result.attention_rules.insert(read<AttentionParameterRule>(row));
        attention_rules_.restore(*entry);
    }
    for (const auto & row : data.at("region_structures")) {
        const auto [entry, inserted] = result.region_structures.insert(read<TemplateRegionStructure>(row));
        structures_.restore(*entry);
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
