#include "markov/performance/kernel_cost_model.hpp"

#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace markov {
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

KernelCostModel::KernelCostModel(const nlohmann::json & entries) {
    for (const auto & entry : entries) {
        const auto features = KernelCostFeatures::read(entry.at("features"));
        const auto duration = input::integer(entry.at("estimate_ns"), 0, std::numeric_limits<std::int64_t>::max());
        auto & launches = entries_[{ features.tensor_descriptor(), features.attention, features.operands }];
        if (std::ranges::any_of(launches,
                                [&](const Launch & launch) { return launch.task_type == features.task_type && launch.blocks == features.launch_blocks; }))
            throw std::invalid_argument("duplicate kernel cost features");
        launches.push_back({ features.task_type, features.launch_blocks, duration, features.tensor_traversal });
    }
}

std::optional<std::int64_t> KernelCostModel::measured_cost(const KernelCostFeatures & features) const {
    const auto found = entries_.find({ features.tensor_descriptor(), features.attention, features.operands });
    if (found == entries_.end()) return std::nullopt;
    for (const auto & launch : found->second)
        if (launch.task_type == features.task_type && launch.blocks == features.launch_blocks && launch.tensor_traversal == features.tensor_traversal)
            return launch.duration_ns;
    return std::nullopt;
}

CostModelResult KernelCostModel::estimate(const CostQuery & query) const {
    if (query.kind != CostQueryKind::Kernel || query.issue != CostQueryIssue::None) return std::unexpected(CostModelGap{});
    const auto * features = &std::get<KernelCostQuery>(query.features);

    auto found = entries_.find({ features->tensors, features->attention, features->operands });
    if (found == entries_.end() && !features->operands.empty()) {
        // Only an existing producer-declared tensor traversal may omit the
        // value operands. A physical-only non-traversal sample cannot match.
        found = entries_.find({ features->tensors, features->attention, {} });
        if (found != entries_.end() && std::ranges::none_of(found->second, &Launch::tensor_traversal)) found = entries_.end();
    }
    if (found == entries_.end()) return std::unexpected(CostModelGap{});
    if (found->second.size() != 1) return std::unexpected(CostModelGap{ CostQueryIssue::FeaturesUndefined, "kernel_launch_mapping_undefined" });

    // The producer's traversal assumption can explicitly project semantic
    // values out of this key. Other unmodeled loops/indexing remain gaps.
    const auto & launch = found->second.front();
    if (!launch.tensor_traversal && !features->unmodeled_operands.empty())
        return std::unexpected(CostModelGap{ CostQueryIssue::FeaturesUndefined, "kernel_operand_projection_undefined" });
    return launch.duration_ns;
}

} // namespace markov
