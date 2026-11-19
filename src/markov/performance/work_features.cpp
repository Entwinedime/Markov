#include "markov/performance/work_features.hpp"

#include <algorithm>
#include <iterator>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov {

WorkFeatures::WorkFeatures(Parameters parameters)
    : parameters_(std::make_move_iterator(parameters.begin()), std::make_move_iterator(parameters.end())) {}

WorkFeatures::WorkFeatures(const GraphScope & scope) {
    const auto & info = scope.info;
    if (!info.parameters.contains("members")) {

        // Local calls declare their own inputs. The enclosing batch can identify the call without driving its cost.
        for (const auto & [name, value] : info.parameters) {
            if (!info.constants.contains(name)) parameters_.emplace(name, value);
        }
        return;
    }

    std::set<std::string> geometry;

    // Metadata describes backing storage. Data ports and local guards describe the call's actual shape.
    for (const auto & port : scope.ports) {
        if (port.use != PortUse::Data || !port.tensor || !port.tensor->shape_rule) continue;
        for (const auto & dimension : *port.tensor->shape_rule) geometry.merge(dimension.variables());
    }
    for (const auto & guard : info.guards) geometry.merge(guard.variables());

    // Inherited values enter the cost coordinate only when the work's own ports or guards use them.
    for (const auto & name : geometry) parameters_.emplace(name, info.parameters.at(name));
    const auto batch = info.work.find("members");
    if (batch == info.work.end()) return;
    if (const auto phase = info.work.find("phase"); phase != info.work.end()) phase_ = phase->get_ref<const std::string &>();

    // A local KV read gives the work an independent context-length and attention coordinate.
    const bool reads_kv = std::ranges::any_of(scope.kv_accesses, [](const auto & access) {
        return access.value.access == StateAccess::Read;
    });
    batch_axis_count_ = reads_kv ? BatchAxisCount : ContextTokens;
    for (const auto & member : batch->get_ref<const nlohmann::json::array_t &>()) {
        const auto query = member.at("new_tokens").get<double>();
        const auto context = member.at("context_tokens").get<double>();
        if (query < 0 || context < 0) throw std::invalid_argument("cost geometry has negative token lengths");

        ++batch_[Members];
        batch_[QueryTokens] += query;
        if (!reads_kv) continue;
        batch_[ContextTokens] += context;
        batch_[MaxContextTokens] = std::max(batch_[MaxContextTokens], context);
        batch_[MaxQueryTokens] = std::max(batch_[MaxQueryTokens], query);
        batch_[AttentionPairs] += query * (context + (query + 1) / 2);
    }
}

std::optional<double> WorkFeatures::value(std::string_view axis) const {
    if (axis.starts_with(parameter_prefix_)) {
        const auto parameter = parameters_.find(axis.substr(parameter_prefix_.size()));
        if (parameter == parameters_.end()) return {};
        return static_cast<double>(parameter->second);
    }
    const auto axes = batch_axes();
    const auto batch = std::ranges::find(axes, axis);
    if (batch == axes.end()) return {};
    return batch_[static_cast<std::size_t>(batch - axes.begin())];
}

nlohmann::json WorkFeatures::document() const {
    auto axes = nlohmann::json::object();
    const auto names = batch_axes();
    for (std::size_t index = 0; index < names.size(); ++index) axes[names[index]] = batch_[index];
    for (const auto & [name, value] : parameters_) axes[std::string(parameter_prefix_) + name] = value;
    return {{ "phase", phase_ }, { "axes", std::move(axes) }};
}

} // namespace markov
