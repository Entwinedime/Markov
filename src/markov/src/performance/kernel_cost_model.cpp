#include "markov/performance/kernel_cost_model.hpp"

#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>

namespace markov {
namespace {

// Both the sampled descriptor and the transformed kernel use this field set.
// No source span, request identity or source shape enters the target lookup.
std::array<std::string, 8> descriptor(const auto & kernel) {
    return { kernel.name,         kernel.operation,     kernel.input_shapes,  kernel.output_shapes,
             kernel.input_dtypes, kernel.output_dtypes, kernel.input_formats, kernel.output_formats };
}

} // namespace

KernelCostFeatures KernelCostFeatures::read(const nlohmann::json & data) {
    // Empty tensor slots and vendor punctuation are meaningful. Keep them
    // unchanged; normalization belongs to the producing runtime adapter.
    return { input::string_value(data.at("name")),        input::string_value(data.at("operation")),
             data.at("input_shapes").get<std::string>(),  data.at("output_shapes").get<std::string>(),
             data.at("input_dtypes").get<std::string>(),  data.at("output_dtypes").get<std::string>(),
             data.at("input_formats").get<std::string>(), data.at("output_formats").get<std::string>(),
             input::string_value(data.at("task_type")),   input::integer(data.at("launch_blocks"), 0, std::numeric_limits<std::int64_t>::max()) };
}

nlohmann::json KernelCostFeatures::describe() const {
    return {
        {           "name",           name },
        {      "operation",      operation },
        {   "input_shapes",   input_shapes },
        {  "output_shapes",  output_shapes },
        {   "input_dtypes",   input_dtypes },
        {  "output_dtypes",  output_dtypes },
        {  "input_formats",  input_formats },
        { "output_formats", output_formats },
        {      "task_type",      task_type },
        {  "launch_blocks",  launch_blocks }
    };
}

KernelCostModel::KernelCostModel(const nlohmann::json & entries) {
    for (const auto & entry : entries) {
        const auto features = KernelCostFeatures::read(entry.at("features"));
        const auto duration = input::integer(entry.at("estimate_ns"), 0, std::numeric_limits<std::int64_t>::max());
        auto & launches = entries_[descriptor(features)];
        if (std::ranges::any_of(launches,
                                [&](const Launch & launch) { return launch.task_type == features.task_type && launch.blocks == features.launch_blocks; }))
            throw std::invalid_argument("duplicate kernel cost features");
        launches.push_back({ features.task_type, features.launch_blocks, duration });
    }
}

std::optional<std::int64_t> KernelCostModel::measured_cost(const KernelCostFeatures & features) const {
    const auto found = entries_.find(descriptor(features));
    if (found == entries_.end()) return std::nullopt;
    for (const auto & launch : found->second)
        if (launch.task_type == features.task_type && launch.blocks == features.launch_blocks) return launch.duration_ns;
    return std::nullopt;
}

std::optional<std::int64_t> KernelCostModel::estimate(const ServiceWork & work) const {
    if (work.node_kind != SourceSpanKind::Compute || !work.kernel) return std::nullopt;

    // Attention, KV and other semantic operands are real target work. This
    // descriptor-only candidate has no measurement mapping for those operands.
    if (work.kernel->has_semantic_work()) return std::nullopt;
    const auto found = entries_.find(descriptor(*work.kernel));
    if (found == entries_.end() || found->second.size() != 1) return std::nullopt;
    return found->second.front().duration_ns;
}

} // namespace markov
