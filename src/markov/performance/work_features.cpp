#include "markov/performance/work_features.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov {

WorkFeatures::WorkFeatures(const GraphScope & scope) {
    std::set<std::string> geometry;
    for (const auto & port : scope.ports) {
        if (!port.tensor || !port.tensor->shape_rule) continue;
        for (const auto & dimension : *port.tensor->shape_rule) geometry.merge(dimension.variables());
    }
    for (const auto & guard : scope.info.guards) geometry.merge(guard.variables());

    // Inherited values enter the cost coordinate only when the work's own ports or guards use them.
    for (const auto & name : geometry) parameters_.emplace(name, scope.info.parameters.at(name));
    if (const auto phase = scope.info.work.find("phase"); phase != scope.info.work.end())
        phase_ = phase->get_ref<const std::string &>();
    const auto batch = scope.info.work.find("members");
    if (batch == scope.info.work.end()) return;

    // Request identities stay in the scheduler. Costs use the actual per-member query and context.
    for (const auto & member : batch->get_ref<const nlohmann::json::array_t &>()) {
        const auto query = member.at("new_tokens").get<double>();
        const auto context = member.at("context_tokens").get<double>();
        if (query < 0 || context < 0) throw std::invalid_argument("cost geometry has negative token lengths");
        ++members_;
        query_tokens_ += query;
        context_tokens_ += context;
        max_context_ = std::max(max_context_, context);
        attention_pairs_ += query * (context + (query + 1) / 2);
    }
}

nlohmann::json WorkFeatures::document() const {
    nlohmann::json axes{
        { "batch.members", members_ },
        { "batch.query_tokens", query_tokens_ },
        { "batch.context_tokens", context_tokens_ },
        { "batch.max_context_tokens", max_context_ },
        { "batch.attention_pairs", attention_pairs_ },
    };
    for (const auto & [name, value] : parameters_) axes["parameter." + name] = value;
    return {{ "phase", phase_ }, { "axes", std::move(axes) }};
}

} // namespace markov
