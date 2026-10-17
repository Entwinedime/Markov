#include "markov/performance/cost_query.hpp"

#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <utility>

namespace markov {
namespace {

std::array<std::string, 8> tensor_descriptor(const auto & kernel) {
    return { kernel.name,         kernel.operation,     kernel.input_shapes,  kernel.output_shapes,
             kernel.input_dtypes, kernel.output_dtypes, kernel.input_formats, kernel.output_formats };
}

} // namespace

std::array<std::string, 8> KernelCostFeatures::tensor_descriptor() const { return markov::tensor_descriptor(*this); }

KernelOperandFeatures KernelOperandFeatures::from_work(const SourceKernel & work) {
    KernelOperandFeatures result;
    if (work.positions) {
        // Prefix values change the generated positions, not the loop lengths.
        result.position_lengths.emplace();
        for (const auto & sequence : work.positions->sequences) result.position_lengths->push_back(sequence.input_tokens);
    }

    result.logits_rows = work.logits_token_indices;
    result.request_pool = work.request_pool;
    result.batch_filter = work.batch_filter;
    return result;
}

KernelOperandFeatures KernelOperandFeatures::read(const nlohmann::json & data) {
    KernelOperandFeatures result;
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (data.contains("position_lengths")) {
        result.position_lengths.emplace();
        for (const auto & length : data.at("position_lengths")) result.position_lengths->push_back(input::integer(length, 1, maximum));
    }

    if (data.contains("logits_rows")) {
        result.logits_rows.emplace();
        for (const auto & row : data.at("logits_rows")) result.logits_rows->push_back(input::integer(row, 0, maximum));
    }

    if (data.contains("request_pool")) result.request_pool = RequestPoolWork::read(data.at("request_pool"));

    if (data.contains("batch_filter")) {
        const auto & filter = data.at("batch_filter");
        const auto input_count = input::integer(filter.at("input_count"), 0, maximum);
        BatchFilter selection{ static_cast<std::size_t>(input_count), {} };
        for (const auto & index : filter.at("retained_indices"))
            selection.retained_indices.push_back(static_cast<std::size_t>(input::integer(index, 0, input_count - 1)));
        result.batch_filter = std::move(selection);
    }
    return result;
}

nlohmann::json KernelOperandFeatures::describe() const {
    auto result = nlohmann::json::object();
    if (position_lengths) result["position_lengths"] = *position_lengths;
    if (logits_rows) result["logits_rows"] = *logits_rows;
    if (request_pool) result["request_pool"] = request_pool->describe();
    if (batch_filter) result["batch_filter"] = batch_filter->describe();
    return result;
}

bool KernelOperandFeatures::empty() const { return !position_lengths && !logits_rows && !request_pool && !batch_filter; }

KernelCostQuery KernelCostQuery::from_work(const SourceKernel & work) {
    KernelCostQuery query{ tensor_descriptor(work),
                           work.attention ? std::optional(AttentionCostFeatures::from_work(*work.attention)) : std::nullopt,
                           KernelOperandFeatures::from_work(work),
                           {},
                           &work };
    if (work.kv_allocation) query.unmodeled_operands.push_back("kv_allocation");
    if (work.kv_release) query.unmodeled_operands.push_back("kv_release");
    if (work.batch_merge) query.unmodeled_operands.push_back("batch_merge");
    if (work.page_table) query.unmodeled_operands.push_back("page_table");
    if (work.sequence_lengths) query.unmodeled_operands.push_back("sequence_lengths");
    if (work.sequence_length_max) query.unmodeled_operands.push_back("sequence_length_max");
    if (work.kv_new_pages) query.unmodeled_operands.push_back("kv_new_pages");
    return query;
}

nlohmann::json KernelCostQuery::describe() const {
    nlohmann::json result{
        {           "name",                         tensors[0] },
        {      "operation",                         tensors[1] },
        {   "input_shapes",                         tensors[2] },
        {  "output_shapes",                         tensors[3] },
        {   "input_dtypes",                         tensors[4] },
        {  "output_dtypes",                         tensors[5] },
        {  "input_formats",                         tensors[6] },
        { "output_formats",                         tensors[7] },
        { "launch_variant", "unique_observed_variant_required" }
    };
    if (attention) result["attention_work"] = attention->describe();
    if (!operands.empty()) result["operand_work"] = operands.describe();
    if (!unmodeled_operands.empty()) {
        auto unmodeled = nlohmann::json::object();
        const auto description = work->describe();
        for (const auto & name : unmodeled_operands) unmodeled[name] = description.at(name);
        result["unmodeled_operands"] = std::move(unmodeled);
    }
    return result;
}

UnresolvedCostQuery UnresolvedCostQuery::from_work(const ServiceWork & work) { return { work.node_kind }; }

nlohmann::json UnresolvedCostQuery::describe() const {
    return {
        { "node_kind", SourceGraph::span_name(node_kind) }
    };
}

std::string_view CostObject::kind_name(CostQueryKind kind) {
    switch (kind) {
    case CostQueryKind::Undefined:
        return "undefined";
    case CostQueryKind::Kernel:
        return "kernel";
    case CostQueryKind::Communication:
        return "communication";
    case CostQueryKind::Transfer:
        return "transfer";
    case CostQueryKind::DeviceCommand:
        return "device_command";
    }
    std::unreachable();
}

nlohmann::json CostObject::describe() const {
    nlohmann::json name = nullptr;
    nlohmann::json boundary = nullptr;
    nlohmann::json values = nullptr;
    if (const auto * value = std::get_if<KernelCostQuery>(&features)) {
        name = value->tensors[0];
        boundary = "physical_compute_task";
        values = value->describe();
    }
    else if (const auto * value = std::get_if<CommunicationCostFeatures>(&features)) {
        name = value->operation;
        boundary = "physical_communication_task";
        values = value->describe();
    }
    else if (const auto * value = std::get_if<TransferCostFeatures>(&features)) {
        name = SourceTransfer::direction_name(value->direction);
        boundary = value->execution == TransferExecution::DeviceAsync ? "asynchronous_device_transfer_service" : "synchronous_host_driver_transfer_service";
        values = value->describe();
    }
    else if (const auto * value = std::get_if<DeviceCommandWork>(&features)) {
        name = value->task_type;
        boundary = "physical_device_command";
        values = value->describe();
    }

    else if (const auto * value = std::get_if<UnresolvedCostQuery>(&features)) { values = value->describe(); }

    std::string_view status = "complete";
    if (issue == CostQueryIssue::FeaturesUndefined) status = "features_undefined";
    return {
        {                 "kind",                                                               kind_name(kind) },
        {                 "name",                                                               std::move(name) },
        { "measurement_boundary",                                                           std::move(boundary) },
        {             "features",                                                             std::move(values) },
        {             "contract",                                                                        status },
        {         "contract_gap", contract_gap.empty() ? nlohmann::json(nullptr) : nlohmann::json(contract_gap) }
    };
}

CostQuery CostQuery::from_work(const ServiceWork & work) {
    CostQuery result;
    auto & objects = result.objects;
    if (work.node_kind == SourceSpanKind::CpuControl) {
        if (work.transfer)
            objects.push_back({
                CostQueryKind::Transfer,
                TransferCostFeatures{ work.transfer->bytes, work.transfer->direction, TransferExecution::SynchronousHostDriver }
            });
        // Control-service time is deliberately outside the current model.
        return result;
    }

    if (work.node_kind == SourceSpanKind::Compute) {
        if (!work.kernel)
            objects.push_back({ CostQueryKind::Kernel, UnresolvedCostQuery::from_work(work), CostQueryIssue::FeaturesUndefined, "kernel_work_undefined" });
        else {
            // An unmeasured physical key is a data gap. Value-operand projection
            // is qualified by the candidate policy during lookup; absence of a
            // candidate does not prove that this projection is unsupported.
            objects.push_back({ CostQueryKind::Kernel, KernelCostQuery::from_work(*work.kernel) });
        }
        return result;
    }

    if (work.communication && (work.node_kind == SourceSpanKind::Dma || work.node_kind == SourceSpanKind::DeviceMarker)) {
        const auto & task = *work.communication;
        objects.push_back({
            CostQueryKind::Communication,
            CommunicationCostFeatures{ work.node_kind, task.operation, task.bytes, task.data_type, task.transport, task.source_rank, task.destination_rank }
        });
    }
    else if (work.node_kind == SourceSpanKind::Dma && work.transfer) {
        objects.push_back({
            CostQueryKind::Transfer,
            TransferCostFeatures{ work.transfer->bytes, work.transfer->direction, TransferExecution::DeviceAsync }
        });
    }
    else if (work.node_kind == SourceSpanKind::DeviceMarker && work.device_command) {
        objects.push_back({ CostQueryKind::DeviceCommand, *work.device_command });
    }
    else {
        objects.push_back(
            { CostQueryKind::Undefined, UnresolvedCostQuery::from_work(work), CostQueryIssue::FeaturesUndefined, "service_work_features_undefined" });
    }
    return result;
}

} // namespace markov
