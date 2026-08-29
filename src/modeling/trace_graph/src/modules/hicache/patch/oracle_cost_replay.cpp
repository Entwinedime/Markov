/**
 * @file
 * @brief Diagnostic-only target-observed cost injection after HiCache structure replay.
 */
#include "markov/trace_graph/modules/hicache/patch/io_resource_model.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace markov::trace_graph::modules::hicache::patch {

namespace oracle_cost_replay_detail {

using Json = nlohmann::json;

constexpr std::string_view kPrimitiveControl = "host_control_primitive";
constexpr std::string_view kOutcomeOnlyControl = "outcome_only_terminal_control";

struct OracleCost {
    std::string effect_id;
    std::string effect_type;
    std::string direction;
    std::string resource_scope;
    std::string resource_lane;
    std::string control_semantics;
    uint64_t logical_order_epoch = 0;
    uint64_t operation_count = 0;
    uint64_t page_count = 0;
    uint64_t byte_count = 0;
    uint64_t service_us = 0;
    uint64_t control_us = 0;
    uint64_t observed_blocking_us = 0;
};

uint64_t exact_u64(const Json & object, std::string_view field) {
    const auto key = std::string(field);
    if (!object.contains(key) || !object.at(key).is_number_unsigned()) {
        throw std::invalid_argument("oracle-cost replay field must be an unsigned integer: " + key);
    }
    return object.at(key).get<uint64_t>();
}

std::string exact_string(const Json & object, std::string_view field) {
    const auto key = std::string(field);
    if (!object.contains(key) || !object.at(key).is_string()) { throw std::invalid_argument("oracle-cost replay field must be a string: " + key); }
    return object.at(key).get<std::string>();
}

OracleCost parse_cost(const Json & raw) {
    if (!raw.is_object()) throw std::invalid_argument("oracle-cost replay costs must contain objects");
    OracleCost cost{
        .effect_id = exact_string(raw, "effect_id"),
        .effect_type = exact_string(raw, "effect_type"),
        .direction = exact_string(raw, "direction"),
        .resource_scope = exact_string(raw, "resource_scope"),
        .resource_lane = exact_string(raw, "resource_lane"),
        .control_semantics = exact_string(raw, "control_semantics"),
        .logical_order_epoch = exact_u64(raw, "logical_order_epoch"),
        .operation_count = exact_u64(raw, "operation_count"),
        .page_count = exact_u64(raw, "page_count"),
        .byte_count = exact_u64(raw, "byte_count"),
        .service_us = exact_u64(raw, "service_us"),
        .control_us = exact_u64(raw, "control_us"),
        .observed_blocking_us = exact_u64(raw, "observed_blocking_us"),
    };
    if (cost.effect_id.empty()) throw std::invalid_argument("oracle-cost replay effect_id must not be empty");
    if (cost.control_semantics != kPrimitiveControl && cost.control_semantics != kOutcomeOnlyControl) {
        throw std::invalid_argument("oracle-cost replay contains an unknown control_semantics for effect: " + cost.effect_id);
    }
    return cost;
}

std::vector<OracleCost> load_costs(const std::string & filename) {
    std::ifstream stream(filename);
    if (!stream) throw std::invalid_argument("cannot open oracle-cost replay input: " + filename);
    Json root;
    try {
        stream >> root;
    }
    catch (const Json::exception & error) {
        throw std::invalid_argument("invalid oracle-cost replay JSON: " + std::string(error.what()));
    }
    if (!root.is_object()) throw std::invalid_argument("oracle-cost replay root must be an object");
    for (std::string_view forbidden : { "target_e2e_us", "actual_e2e_us", "target_prediction_us" }) {
        if (root.contains(std::string(forbidden))) { throw std::invalid_argument("oracle-cost replay must not consume target E2E fields"); }
    }
    if (!root.contains("costs") || !root.at("costs").is_array()) { throw std::invalid_argument("oracle-cost replay costs must be an array"); }
    std::vector<OracleCost> costs;
    costs.reserve(root.at("costs").size());
    for (const auto & raw : root.at("costs")) costs.push_back(parse_cost(raw));
    return costs;
}

bool requires_override(const HiCacheIoCostRecord & cost) {
    return cost.status == HiCacheIoCostStatus::Ready && cost.direction != model::HiCacheTransferDirection::None
           && (cost.effective_byte_count > 0 || cost.zero_payload_control);
}

void checked_accumulate(uint64_t & total, uint64_t value, const char * message) { total = core::checked_add_u64(total, value, message); }

void validate_shape(const HiCacheIoCostRecord & model_cost, const OracleCost & oracle, uint64_t kv_bytes_per_page) {
    const auto expected_effect_type = model::hicache_effect_type_name(model_cost.effect_type);
    const auto expected_direction = model::hicache_transfer_direction_name(model_cost.direction);
    const auto expected_pages = kv_bytes_per_page == 0 ? 0 : model_cost.effective_byte_count / kv_bytes_per_page;
    if (oracle.effect_type != expected_effect_type || oracle.direction != expected_direction || oracle.operation_count != model_cost.operation_count
        || oracle.page_count != expected_pages || oracle.byte_count != model_cost.effective_byte_count || oracle.resource_scope != model_cost.resource_scope
        || oracle.resource_lane != model_cost.resource_lane || oracle.logical_order_epoch != model_cost.logical_order_epoch) {
        throw std::invalid_argument("oracle-cost replay operation shape mismatch for effect: " + model_cost.effect_id);
    }
    const bool primitive_control_supported =
        model_cost.zero_payload_control || model_cost.effect_type == model::HiCacheEffectType::Loadback
        || model_cost.effect_type == model::HiCacheEffectType::CommitDeviceToHost;
    const auto expected_semantics = primitive_control_supported ? kPrimitiveControl : kOutcomeOnlyControl;
    if (oracle.control_semantics != expected_semantics) {
        throw std::invalid_argument("oracle-cost replay control semantics mismatch for effect: " + model_cost.effect_id);
    }
}

} // namespace oracle_cost_replay_detail

void apply_hicache_oracle_cost_replay(HiCacheIoResourcePlan & plan, const std::string & filename) {
    using namespace oracle_cost_replay_detail;
    if (filename.empty()) return;

    const auto oracle_costs = load_costs(filename);
    std::unordered_map<std::string, const OracleCost *> oracle_by_effect;
    oracle_by_effect.reserve(oracle_costs.size());
    for (const auto & cost : oracle_costs) {
        if (!oracle_by_effect.emplace(cost.effect_id, &cost).second) {
            throw std::invalid_argument("duplicate oracle-cost replay effect_id: " + cost.effect_id);
        }
    }

    HiCacheOracleCostReplayAudit audit{
        .status = "validating",
        .supplied_cost_count = static_cast<uint64_t>(oracle_costs.size()),
    };
    std::unordered_set<std::string> consumed;
    consumed.reserve(oracle_costs.size());
    for (auto & model_cost : plan.costs) {
        if (!requires_override(model_cost)) continue;
        audit.required_cost_count++;
        const auto match = oracle_by_effect.find(model_cost.effect_id);
        if (match == oracle_by_effect.end()) { throw std::invalid_argument("oracle-cost replay is missing required effect: " + model_cost.effect_id); }
        const auto & oracle = *match->second;
        validate_shape(model_cost, oracle, plan.kv_bytes_per_page);
        consumed.insert(model_cost.effect_id);

        checked_accumulate(audit.oracle_service_us, oracle.service_us, "oracle service total exceeds uint64 range");
        checked_accumulate(audit.oracle_control_us, oracle.control_us, "oracle control total exceeds uint64 range");
        checked_accumulate(audit.observed_blocking_us, oracle.observed_blocking_us, "oracle blocking total exceeds uint64 range");
        model_cost.duration_us = oracle.service_us;
        checked_accumulate(audit.applied_service_us, model_cost.duration_us, "applied oracle service total exceeds uint64 range");
        if (oracle.control_semantics == kPrimitiveControl) {
            model_cost.host_control_duration_us = oracle.control_us;
            checked_accumulate(audit.applied_primitive_control_us, model_cost.host_control_duration_us, "applied oracle control total exceeds uint64 range");
        }
        else {
            model_cost.host_control_duration_us = 0;
            checked_accumulate(audit.outcome_only_control_us, oracle.control_us, "outcome-only control total exceeds uint64 range");
        }
        audit.applied_cost_count++;
    }
    if (consumed.size() != oracle_by_effect.size()) {
        for (const auto & [effect_id, unused] : oracle_by_effect) {
            (void)unused;
            if (!consumed.contains(effect_id)) throw std::invalid_argument("oracle-cost replay contains an unknown or non-transfer effect: " + effect_id);
        }
    }
    audit.effect_identity_exact = audit.required_cost_count == audit.supplied_cost_count && audit.applied_cost_count == audit.required_cost_count;
    audit.operation_shape_exact = audit.effect_identity_exact;
    audit.status = audit.effect_identity_exact ? "ready" : "invalid";
    plan.oracle_cost_replay = std::move(audit);
}

} // namespace markov::trace_graph::modules::hicache::patch
