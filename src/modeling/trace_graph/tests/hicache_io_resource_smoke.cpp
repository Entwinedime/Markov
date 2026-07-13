/**
 * @file
 * @brief Focused executable tests for HiCache byte costs and logical I/O lanes.
 */
#include "markov/trace_graph/modules/hicache/patch/io_resource_model.hpp"

#include <limits>
#include <stdexcept>
#include <string>

namespace {

using markov::trace_graph::modules::hicache::model::hicache_source_carrier_state_name;
using markov::trace_graph::modules::hicache::model::HiCacheEffectDecision;
using markov::trace_graph::modules::hicache::model::HiCacheEffectDecisionLedger;
using markov::trace_graph::modules::hicache::model::HiCacheEffectType;
using markov::trace_graph::modules::hicache::model::HiCacheSourceCarrierState;
using markov::trace_graph::modules::hicache::model::HiCacheTargetEffectState;
using markov::trace_graph::modules::hicache::model::HiCacheTransferDirection;
using markov::trace_graph::modules::hicache::patch::build_hicache_io_resource_plan;
using markov::trace_graph::modules::hicache::patch::HiCacheIoCostStatus;

void expect(bool condition, const std::string & message) {
    if (!condition) throw std::runtime_error(message);
}

HiCacheEffectDecision decision(std::string id, HiCacheEffectType effect_type, HiCacheTransferDirection direction, uint64_t bytes, uint64_t epoch,
                               HiCacheTargetEffectState state = HiCacheTargetEffectState::Required, std::string cache_scope = "scope:1") {
    return HiCacheEffectDecision{
        .effect_key = std::move(id),
        .effect_type = effect_type,
        .direction = direction,
        .cache_scope = std::move(cache_scope),
        .candidate_page_count = bytes > 0 ? 1U : 0U,
        .effective_page_count = bytes > 0 ? 1U : 0U,
        .effective_byte_count = bytes,
        .eligibility_boundary = { .kind = "eligibility", .epoch = epoch },
        .resource_lane = "untrusted_upstream_lane",
        .target_effect_state = state,
    };
}

HiCacheEffectDecisionLedger configured_ledger() {
    HiCacheEffectDecisionLedger ledger;
    ledger.kv_bytes_per_page = 1;
    ledger.byte_projection_available = true;
    ledger.byte_projection_source = "smoke";
    ledger.io_model_id = "smoke";
    ledger.io_model_digest = "sha256_json:smoke";
    ledger.io_model_calibration_status = "calibrated";
    ledger.resource_model = "scope_local_directional_device_host_shared_host_storage_v1";
    ledger.device_host_bandwidth_bytes_per_sec = 5;
    ledger.host_storage_bandwidth_bytes_per_sec = 3;
    ledger.io_model_provenance = {
        {  "device_host_bandwidth", "smoke" },
        { "host_storage_bandwidth", "smoke" },
        {            "kv_geometry", "smoke" },
    };
    return ledger;
}

void check_duration_and_lane_ordering() {
    auto ledger = configured_ledger();
    ledger.decisions = {
        decision("commit:b", HiCacheEffectType::CommitHostToStorage, HiCacheTransferDirection::HostToStorage, 3, 2),
        decision("loadback:c", HiCacheEffectType::Loadback, HiCacheTransferDirection::HostToDevice, 10, 1),
        decision("prefetch:a", HiCacheEffectType::PrefetchIo, HiCacheTransferDirection::StorageToHost, 1, 1),
        decision("commit:d2h", HiCacheEffectType::CommitDeviceToHost, HiCacheTransferDirection::DeviceToHost, 5, 1),
        decision("prefetch:no-op", HiCacheEffectType::PrefetchIo, HiCacheTransferDirection::StorageToHost, 0, 3, HiCacheTargetEffectState::NotRequired),
        decision("gate:d", HiCacheEffectType::CommitCapacityGate, HiCacheTransferDirection::None, 0, 4),
        decision("prefetch:z", HiCacheEffectType::PrefetchIo, HiCacheTransferDirection::StorageToHost, 1, 1, HiCacheTargetEffectState::Partial),
    };

    const auto plan = build_hicache_io_resource_plan(ledger);
    expect(plan.status == "ready", "complete I/O parameters did not produce a ready resource plan");
    expect(plan.calibrated_for_apply(), "calibrated I/O model did not authorize production apply");
    expect(plan.ready_count() == 6, "ready cost count is incorrect");
    expect(plan.costs[0].duration_us == 1'000'000, "exact host-storage duration is incorrect");
    expect(plan.costs[1].duration_us == 2'000'000, "device-host duration is incorrect");
    expect(plan.costs[2].duration_us == 333'334, "fractional microsecond duration was not rounded up");
    expect(plan.costs[3].duration_us == 1'000'000, "device-to-host duration is incorrect");
    expect(plan.costs[4].status == HiCacheIoCostStatus::NotRequired, "target no-op was treated as an I/O operation");
    expect(plan.costs[5].status == HiCacheIoCostStatus::Ready && plan.costs[5].duration_us == 0, "dependency-only gate did not retain zero duration");
    expect(plan.costs[0].resource_scope == "scope:1", "resource scope was not copied from the target decision");
    expect(plan.costs[0].resource_lane == "scope:1/host_storage_lane", "host-storage lane was taken from the upstream hint");
    expect(plan.costs[1].resource_lane == "scope:1/host_to_device_lane", "host-to-device lane assignment is incorrect");
    expect(plan.costs[3].resource_lane == "scope:1/device_to_host_lane", "device-to-host lane assignment is incorrect");
    expect(plan.costs[5].resource_lane.empty(), "dependency-only gate was assigned to an I/O lane");
    expect(plan.costs[6].status == HiCacheIoCostStatus::Ready, "partial target effect did not receive its effective-byte cost");
    expect(plan.lane_dependencies.size() == 2, "resource plan ordered operations across independent lanes");
    expect(plan.lane_dependencies[0].predecessor_effect_id == "prefetch:a", "same-lane predecessor order is unstable");
    expect(plan.lane_dependencies[0].successor_effect_id == "prefetch:z", "same-boundary effect-id tie-break is unstable");
    expect(plan.lane_dependencies[1].predecessor_effect_id == "prefetch:z", "same-lane predecessor chain is incomplete");
    expect(plan.lane_dependencies[1].successor_effect_id == "commit:b", "same-lane successor order is unstable");
}

void check_scope_local_lane_ordering() {
    auto ledger = configured_ledger();
    ledger.decisions = {
        decision("scope1:prefetch",
                 HiCacheEffectType::PrefetchIo,
                 HiCacheTransferDirection::StorageToHost,
                 1,
                 1,
                 HiCacheTargetEffectState::Required,
                 "scope:1"),
        decision("scope2:prefetch",
                 HiCacheEffectType::PrefetchIo,
                 HiCacheTransferDirection::StorageToHost,
                 1,
                 1,
                 HiCacheTargetEffectState::Required,
                 "scope:2"),
        decision("scope1:commit",
                 HiCacheEffectType::CommitHostToStorage,
                 HiCacheTransferDirection::HostToStorage,
                 1,
                 2,
                 HiCacheTargetEffectState::Required,
                 "scope:1"),
        decision("scope2:commit",
                 HiCacheEffectType::CommitHostToStorage,
                 HiCacheTransferDirection::HostToStorage,
                 1,
                 2,
                 HiCacheTargetEffectState::Required,
                 "scope:2"),
    };

    const auto plan = build_hicache_io_resource_plan(ledger);
    expect(plan.status == "ready", "scope-local resource plan is not ready");
    expect(plan.lane_dependencies.size() == 2, "resource plan serialized operations across independent scopes");
    expect(plan.lane_dependencies[0].resource_lane == "scope:1/host_storage_lane", "first scope lane identity is incorrect");
    expect(plan.lane_dependencies[0].predecessor_effect_id == "scope1:prefetch", "first scope predecessor is incorrect");
    expect(plan.lane_dependencies[0].successor_effect_id == "scope1:commit", "first scope successor is incorrect");
    expect(plan.lane_dependencies[1].resource_lane == "scope:2/host_storage_lane", "second scope lane identity is incorrect");
    expect(plan.lane_dependencies[1].predecessor_effect_id == "scope2:prefetch", "second scope predecessor is incorrect");
    expect(plan.lane_dependencies[1].successor_effect_id == "scope2:commit", "second scope successor is incorrect");
}

void check_contract_only_model() {
    auto ledger = configured_ledger();
    ledger.io_model_calibration_status = "contract_only";
    ledger.decisions = {
        decision("prefetch:contract", HiCacheEffectType::PrefetchIo, HiCacheTransferDirection::StorageToHost, 1, 1),
    };
    const auto plan = build_hicache_io_resource_plan(ledger);
    expect(plan.status == "ready", "contract-only model could not build a shadow cost plan");
    expect(!plan.calibrated_for_apply(), "contract-only model authorized production apply");
}

void check_missing_parameters() {
    auto ledger = configured_ledger();
    ledger.host_storage_bandwidth_bytes_per_sec = 0;
    ledger.decisions = { decision("prefetch:missing", HiCacheEffectType::PrefetchIo, HiCacheTransferDirection::StorageToHost, 1, 1) };
    const auto missing_bandwidth = build_hicache_io_resource_plan(ledger);
    expect(missing_bandwidth.status == "blocked", "missing bandwidth did not block the resource plan");
    expect(missing_bandwidth.costs[0].status == HiCacheIoCostStatus::MissingBandwidth, "missing bandwidth classification is incorrect");

    ledger = configured_ledger();
    ledger.byte_projection_available = false;
    ledger.decisions = { decision("prefetch:projection", HiCacheEffectType::PrefetchIo, HiCacheTransferDirection::StorageToHost, 0, 1) };
    ledger.decisions[0].effective_page_count = 1;
    const auto missing_projection = build_hicache_io_resource_plan(ledger);
    expect(missing_projection.costs[0].status == HiCacheIoCostStatus::MissingByteProjection, "missing projection classification is incorrect");

    ledger = configured_ledger();
    ledger.decisions = { decision("loadback:scope", HiCacheEffectType::Loadback, HiCacheTransferDirection::HostToDevice, 1, 1) };
    ledger.decisions[0].cache_scope.clear();
    const auto missing_scope = build_hicache_io_resource_plan(ledger);
    expect(missing_scope.costs[0].status == HiCacheIoCostStatus::MissingResourceScope, "missing resource scope did not block a transfer");

    ledger = configured_ledger();
    ledger.io_model_id.clear();
    ledger.decisions = { decision("loadback:identity", HiCacheEffectType::Loadback, HiCacheTransferDirection::HostToDevice, 1, 1) };
    const auto missing_identity = build_hicache_io_resource_plan(ledger);
    expect(missing_identity.costs[0].status == HiCacheIoCostStatus::MissingIoModelIdentity, "missing model identity did not block a traceable duration");

    ledger = configured_ledger();
    ledger.io_model_provenance.clear();
    ledger.decisions = { decision("loadback:provenance", HiCacheEffectType::Loadback, HiCacheTransferDirection::HostToDevice, 1, 1) };
    const auto missing_provenance = build_hicache_io_resource_plan(ledger);
    expect(missing_provenance.costs[0].status == HiCacheIoCostStatus::MissingIoModelProvenance, "missing model provenance did not block a traceable duration");

    ledger = configured_ledger();
    ledger.resource_model.clear();
    ledger.decisions = { decision("loadback:resource-model", HiCacheEffectType::Loadback, HiCacheTransferDirection::HostToDevice, 1, 1) };
    const auto missing_resource_model = build_hicache_io_resource_plan(ledger);
    expect(missing_resource_model.costs[0].status == HiCacheIoCostStatus::MissingResourceModel, "missing resource model did not block a traceable duration");

    ledger = configured_ledger();
    ledger.decisions = { decision("prefetch:overflow", HiCacheEffectType::PrefetchIo, HiCacheTransferDirection::StorageToHost, 0, 1) };
    ledger.decisions[0].effective_page_count = 1;
    const auto projection_overflow = build_hicache_io_resource_plan(ledger);
    expect(projection_overflow.costs[0].status == HiCacheIoCostStatus::ByteProjectionOverflow, "byte projection overflow was classified as a missing input");
}

void check_duration_overflow() {
    auto ledger = configured_ledger();
    ledger.device_host_bandwidth_bytes_per_sec = 1;
    ledger.decisions = {
        decision("loadback:overflow", HiCacheEffectType::Loadback, HiCacheTransferDirection::HostToDevice, std::numeric_limits<uint64_t>::max(), 1),
    };
    const auto plan = build_hicache_io_resource_plan(ledger);
    expect(plan.costs[0].status == HiCacheIoCostStatus::DurationOverflow, "overflowing duration was accepted");
    expect(plan.costs[0].duration_us == 0, "overflowing duration produced an executable cost");
}

void check_explicit_contract_states() {
    auto ledger = configured_ledger();
    ledger.decisions = {
        decision("prefetch:deferred", HiCacheEffectType::PrefetchIo, HiCacheTransferDirection::StorageToHost, 0, 1, HiCacheTargetEffectState::Deferred),
        decision("prefetch:unresolved", HiCacheEffectType::PrefetchIo, HiCacheTransferDirection::StorageToHost, 0, 2, HiCacheTargetEffectState::Unresolved),
    };
    const auto plan = build_hicache_io_resource_plan(ledger);
    expect(plan.costs[0].status == HiCacheIoCostStatus::Deferred, "deferred target effect was treated as a transfer");
    expect(plan.costs[1].status == HiCacheIoCostStatus::Unresolved, "unresolved target effect was treated as a transfer");
    expect(hicache_source_carrier_state_name(HiCacheSourceCarrierState::NotEvaluated) == "not_evaluated",
           "not-evaluated source carrier state name is unstable");
    expect(hicache_source_carrier_state_name(HiCacheSourceCarrierState::Present) == "present", "present source carrier state name is unstable");
    expect(hicache_source_carrier_state_name(HiCacheSourceCarrierState::Absent) == "absent", "absent source carrier state name is unstable");
    expect(hicache_source_carrier_state_name(HiCacheSourceCarrierState::Unobservable) == "unobservable", "unobservable source carrier state name is unstable");
    expect(hicache_source_carrier_state_name(HiCacheSourceCarrierState::Ambiguous) == "ambiguous", "ambiguous source carrier state name is unstable");
}

} // namespace

int main() {
    check_duration_and_lane_ordering();
    check_scope_local_lane_ordering();
    check_contract_only_model();
    check_missing_parameters();
    check_duration_overflow();
    check_explicit_contract_states();
    return 0;
}
