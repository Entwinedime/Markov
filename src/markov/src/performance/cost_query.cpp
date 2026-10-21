#include "markov/performance/cost_query.hpp"

#include "markov/contracts/control.hpp"
#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <stdexcept>
#include <utility>

namespace markov {
namespace {

std::array<std::string, 8> tensor_descriptor(const auto & kernel) {
    return { kernel.name,         kernel.operation,     kernel.input_shapes,  kernel.output_shapes,
             kernel.input_dtypes, kernel.output_dtypes, kernel.input_formats, kernel.output_formats };
}

} // namespace

AttentionCostFeatures AttentionCostFeatures::read(const nlohmann::json & data) {
    const auto role = data.at("role").get<std::string>();
    AttentionOperation operation;
    if (role == "cache_write") operation = AttentionOperation::CacheWrite;
    else if (role == "prefill_attention") operation = AttentionOperation::Prefill;
    else if (role == "decode_attention") operation = AttentionOperation::Decode;
    else throw std::invalid_argument("unknown attention cost role: " + role);

    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    std::vector<AttentionCostSequence> sequences;
    for (const auto & sequence : data.at("sequences"))
        sequences.push_back({ input::integer(sequence.at("query_tokens"), 1, maximum), input::integer(sequence.at("prefix_tokens"), 0, maximum) });
    std::optional<std::int64_t> query_heads;
    if (!data.at("query_heads").is_null()) query_heads = input::integer(data.at("query_heads"), 1, maximum);

    return { operation,
             std::move(sequences),
             query_heads,
             input::integer(data.at("kv_heads"), 1, maximum),
             input::integer(data.at("head_dim"), 1, maximum),
             input::integer(data.at("page_size_tokens"), 1, maximum),
             input::integer(data.at("cache_pages"), 1, maximum) };
}

AttentionCostFeatures AttentionCostFeatures::from_work(const AttentionWorkload & work) {
    std::vector<AttentionCostSequence> sequences;
    for (const auto & sequence : work.sequences) sequences.push_back({ sequence.input_tokens, sequence.prefix_tokens });
    const auto query_heads = work.operation == AttentionOperation::CacheWrite ? std::nullopt : std::optional(work.query_heads);
    return { work.operation, std::move(sequences), query_heads, work.kv_heads, work.head_dim, work.page_size_tokens, work.cache_pages };
}

nlohmann::json AttentionCostFeatures::describe() const {
    std::string_view role;
    switch (operation) {
    case AttentionOperation::CacheWrite:
        role = "cache_write";
        break;
    case AttentionOperation::Prefill:
        role = "prefill_attention";
        break;
    case AttentionOperation::Decode:
        role = "decode_attention";
        break;
    }
    auto requests = nlohmann::json::array();
    for (const auto & sequence : sequences)
        requests.push_back({
            {  "query_tokens",  sequence.query_tokens },
            { "prefix_tokens", sequence.prefix_tokens }
        });
    return {
        {             "role",                                                                 role },
        {        "sequences",                                                             requests },
        {      "query_heads", query_heads ? nlohmann::json(*query_heads) : nlohmann::json(nullptr) },
        {         "kv_heads",                                                             kv_heads },
        {         "head_dim",                                                             head_dim },
        { "page_size_tokens",                                                     page_size_tokens },
        {      "cache_pages",                                                          cache_pages }
    };
}

KernelCostFeatures KernelCostFeatures::read(const nlohmann::json & data) {
    // Empty tensor slots and vendor punctuation are meaningful. Keep them
    // unchanged; normalization belongs to the producing runtime adapter.
    return { input::string_value(data.at("name")),
             input::string_value(data.at("operation")),
             data.at("input_shapes").get<std::string>(),
             data.at("output_shapes").get<std::string>(),
             data.at("input_dtypes").get<std::string>(),
             data.at("output_dtypes").get<std::string>(),
             data.at("input_formats").get<std::string>(),
             data.at("output_formats").get<std::string>(),
             input::string_value(data.at("task_type")),
             input::integer(data.at("launch_blocks"), 0, std::numeric_limits<std::int64_t>::max()),
             data.at("tensor_traversal").get<bool>(),
             data.contains("attention_work") ? std::optional(AttentionCostFeatures::read(data.at("attention_work"))) : std::nullopt,
             data.contains("operand_work") ? KernelOperandFeatures::read(data.at("operand_work")) : KernelOperandFeatures{} };
}

nlohmann::json KernelCostFeatures::describe() const {
    nlohmann::json result{
        {             "name",             name },
        {        "operation",        operation },
        {     "input_shapes",     input_shapes },
        {    "output_shapes",    output_shapes },
        {     "input_dtypes",     input_dtypes },
        {    "output_dtypes",    output_dtypes },
        {    "input_formats",    input_formats },
        {   "output_formats",   output_formats },
        {        "task_type",        task_type },
        {    "launch_blocks",    launch_blocks },
        { "tensor_traversal", tensor_traversal }
    };
    if (attention) result["attention_work"] = attention->describe();
    if (!operands.empty()) result["operand_work"] = operands.describe();
    return result;
}

CommunicationCostFeatures CommunicationCostFeatures::read(const nlohmann::json & data) {
    const auto name = data.at("kind").get<std::string>();
    SourceSpanKind kind;
    if (name == "dma") kind = SourceSpanKind::Dma;
    else if (name == "device_marker") kind = SourceSpanKind::DeviceMarker;
    else throw std::invalid_argument("communication cost requires a physical data or command task");

    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto rank_maximum = std::numeric_limits<std::uint32_t>::max();
    return { kind,
             input::string_value(data.at("operation")),
             static_cast<std::uint64_t>(input::integer(data.at("bytes"), 0, maximum)),
             input::string_value(data.at("data_type")),
             input::string_value(data.at("transport")),
             static_cast<std::uint32_t>(input::integer(data.at("source_rank"), 0, rank_maximum)),
             static_cast<std::uint32_t>(input::integer(data.at("destination_rank"), 0, rank_maximum)) };
}

nlohmann::json CommunicationCostFeatures::describe() const {
    return {
        {             "kind", SourceGraph::span_name(kind) },
        {        "operation",                    operation },
        {            "bytes",                        bytes },
        {        "data_type",                    data_type },
        {        "transport",                    transport },
        {      "source_rank",                  source_rank },
        { "destination_rank",             destination_rank }
    };
}

TransferCostFeatures TransferCostFeatures::read(const nlohmann::json & data) {
    const auto name = data.at("execution").get<std::string>();
    TransferExecution execution;
    if (name == "asynchronous_device") execution = TransferExecution::DeviceAsync;
    else if (name == "synchronous_host_driver") execution = TransferExecution::SynchronousHostDriver;
    else throw std::invalid_argument("unknown transfer execution boundary");

    return { static_cast<std::uint64_t>(input::integer(data.at("bytes"), 0, std::numeric_limits<std::int64_t>::max())),
             SourceTransfer::parse_direction(data.at("direction").get<std::string>()),
             execution };
}

nlohmann::json TransferCostFeatures::describe() const {
    return {
        {     "bytes",                                                                                           bytes },
        { "direction",                                                       SourceTransfer::direction_name(direction) },
        { "execution", execution == TransferExecution::DeviceAsync ? "asynchronous_device" : "synchronous_host_driver" }
    };
}

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
    return { tensor_descriptor(work),
             work.attention ? std::optional(AttentionCostFeatures::from_work(*work.attention)) : std::nullopt,
             KernelOperandFeatures::from_work(work),
             work.value_dependencies };
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
    if (!unmodeled_operands.empty()) result["unmodeled_operands"] = unmodeled_operands;
    return result;
}

nlohmann::json UnresolvedCostQuery::describe() const {
    return {
        { "node_kind", SourceGraph::span_name(node_kind) }
    };
}

std::string_view CostQuery::kind_name(CostQueryKind kind) {
    switch (kind) {
    case CostQueryKind::Undefined:
        return "undefined";
    case CostQueryKind::Kernel:
        return "kernel";
    case CostQueryKind::Communication:
        return "communication";
    case CostQueryKind::Collective:
        return "collective";
    case CostQueryKind::Transfer:
        return "transfer";
    case CostQueryKind::DeviceCommand:
        return "device_command";
    case CostQueryKind::Cpu:
        return "cpu";
    }
    std::unreachable();
}

nlohmann::json CollectiveCostFeatures::describe() const {
    auto result = work.describe();
    result["rank"] = rank;
    result["execution"] = work.host_message ? "host_synchronous" : "rank_device";
    return result;
}

nlohmann::json CostQuery::describe() const {
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
    else if (const auto * value = std::get_if<CollectiveCostFeatures>(&features)) {
        name = value->work.operation;
        boundary = value->work.host_message ? "host_message_rank_completion_envelope" : "collective_rank_device_service";
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

    else if (const auto * value = std::get_if<CpuServiceWork>(&features)) {
        name = "cpu_service";
        boundary = "named_thread_service_interval";
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
    if (work.node_kind == SourceSpanKind::CpuControl) {
        if (work.transfer)
            return {
                CostQueryKind::Transfer,
                TransferCostFeatures{ work.transfer->bytes, work.transfer->direction, TransferExecution::SynchronousHostDriver }
            };
        if (work.cpu) {
            if (work.cpu->step == StepKind::Publish && !work.cpu->publication)
                return { CostQueryKind::Cpu, *work.cpu, CostQueryIssue::FeaturesUndefined, "cpu_publication_work_not_qualified" };
            return { CostQueryKind::Cpu, *work.cpu };
        }
        return { CostQueryKind::Undefined, UnresolvedCostQuery{ work.node_kind }, CostQueryIssue::FeaturesUndefined, "cpu_service_features_not_defined" };
    }
    if (work.collective)
        return {
            CostQueryKind::Collective,
            CollectiveCostFeatures{ *work.collective, work.collective_rank }
        };
    if (work.node_kind == SourceSpanKind::Compute) {
        if (!work.kernel) return { CostQueryKind::Kernel, UnresolvedCostQuery{ work.node_kind }, CostQueryIssue::FeaturesUndefined, "kernel_work_undefined" };
        // Launch and operand policies belong to the selected cost model.
        return { CostQueryKind::Kernel, KernelCostQuery::from_work(*work.kernel) };
    }
    if (work.node_kind == SourceSpanKind::Dma && work.transfer)
        return {
            CostQueryKind::Transfer,
            TransferCostFeatures{ work.transfer->bytes, work.transfer->direction, TransferExecution::DeviceAsync }
        };
    if (work.node_kind == SourceSpanKind::DeviceMarker && work.device_command) return { CostQueryKind::DeviceCommand, *work.device_command };
    return { CostQueryKind::Undefined, UnresolvedCostQuery{ work.node_kind }, CostQueryIssue::FeaturesUndefined, "service_work_features_undefined" };
}

} // namespace markov
