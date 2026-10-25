#include "markov/contracts/expert_routes.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace markov {
namespace {

std::array<std::int64_t, 2> interval(const nlohmann::json & source, const char * field, std::int64_t limit) {
    const auto & range = source.at(field);
    if (!range.is_array() || range.size() != 2 || !range[0].is_number_integer() || !range[1].is_number_integer())
        throw std::invalid_argument(std::string("route ") + field + " needs an integer [begin, end) range");

    const auto begin = range[0].get<std::int64_t>();
    const auto end = range[1].get<std::int64_t>();
    if (begin < 0 || end <= begin || end > limit) throw std::invalid_argument(std::string("route ") + field + " is outside its logical range");
    return { begin, end };
}

} // namespace

ExpertRoutes::ExpertRoutes(const nlohmann::json & source, std::span<const Request> requests, std::span<const ExpertLayer> layers, std::int64_t model_layers,
                           std::int64_t padding_tokens) {
    for (const auto & layer : layers) layers_.emplace(layer.id, layer);
    for (const auto & [name, _] : source.items()) {
        if (name != "assignments" && name != "padding") throw std::invalid_argument("unsupported route condition field: " + name);
    }
    const auto & assignments = source.at("assignments");
    if (!assignments.is_array()) throw std::invalid_argument("route assignments must be an array");

    // Match the controller's stable arrival order without retaining a second request-progress state.
    std::map<std::string, std::pair<std::size_t, std::int64_t>> identities;
    for (const auto & request : Request::ordered(std::vector<Request>(requests.begin(), requests.end()))) {
        identities.emplace(request.id, std::pair{ requests_.size(), request.prompt_tokens + request.output_tokens - 1 });
        requests_.emplace_back();
    }
    for (const auto & assignment : assignments) {
        const auto name = assignment.at("request").get<std::string>();
        const auto found = identities.find(name);
        if (found == identities.end()) throw std::invalid_argument("route refers to an unknown request: " + name);
        const auto [index, tokens] = found->second;
        insert(assignment, requests_[index], tokens, model_layers);
    }
    for (const auto & [name, request] : identities) require_coverage(requests_[request.first], request.second, name);

    if (source.contains("padding")) {
        const auto & padding = source.at("padding");
        if (!padding.is_array()) throw std::invalid_argument("padding routes must be an array");
        for (const auto & assignment : padding) {
            if (assignment.contains("request")) throw std::invalid_argument("padding routes do not belong to a request");
            insert(assignment, padding_, padding_tokens, model_layers);
        }
    }
    if (padding_tokens != 0) require_coverage(padding_, padding_tokens, "padding");
}

void ExpertRoutes::insert(const nlohmann::json & source, LayerSegments & target, std::int64_t tokens, std::int64_t model_layers) const {
    for (const auto & [name, _] : source.items()) {
        if (name != "request" && name != "layers" && name != "tokens" && name != "experts")
            throw std::invalid_argument("unsupported route assignment field: " + name);
    }
    const auto layer_range = interval(source, "layers", model_layers);
    const auto token_range = interval(source, "tokens", tokens);
    const auto & choices = source.at("experts");
    if (!choices.is_array() || !std::ranges::all_of(choices, [](const auto & expert) { return expert.is_number_integer(); }))
        throw std::invalid_argument("route experts must be an integer array");
    const auto experts = choices.get<std::vector<std::int64_t>>();
    if (std::set(experts.begin(), experts.end()).size() != experts.size()) throw std::invalid_argument("one token cannot select the same expert twice");

    bool used = false;
    for (const auto & [id, layer] : layers_) {
        if (id < layer_range[0] || id >= layer_range[1]) continue;
        if (experts.size() != static_cast<std::size_t>(layer.top_k)
            || std::ranges::any_of(experts, [&](const auto expert) { return expert < 0 || expert >= layer.experts; }))
            throw std::invalid_argument("route experts do not match the native layer: " + std::to_string(id));
        target[id].push_back({ token_range[0], token_range[1], experts });
        used = true;
    }
    if (!used) throw std::invalid_argument("route layer range contains no expert layer");
}

void ExpertRoutes::require_coverage(LayerSegments & target, std::int64_t tokens, const std::string & context) const {
    for (const auto & [id, _] : layers_) {
        auto & segments = target[id];
        std::ranges::sort(segments, {}, &Segment::begin);
        std::int64_t covered = 0;
        for (const auto & segment : segments) {
            if (segment.begin != covered) throw std::invalid_argument("route gap or overlap for " + context + ", layer " + std::to_string(id));
            covered = segment.end;
        }
        if (covered != tokens) throw std::invalid_argument("incomplete route for " + context + ", layer " + std::to_string(id));
    }
}

std::vector<std::int64_t> ExpertRoutes::counts(const LayerSegments & routes, std::int64_t layer, std::int64_t begin, std::int64_t end) const {
    std::vector<std::int64_t> result(static_cast<std::size_t>(layers_.at(layer).experts));
    for (const auto & segment : routes.at(layer)) {
        const auto overlap = std::max<std::int64_t>(0, std::min(end, segment.end) - std::max(begin, segment.begin));
        for (const auto expert : segment.experts) result[static_cast<std::size_t>(expert)] += overlap;
    }
    return result;
}

std::vector<std::int64_t> ExpertRoutes::counts(std::size_t request, std::int64_t layer, std::int64_t begin, std::int64_t end) const {
    return counts(requests_.at(request), layer, begin, end);
}

std::vector<std::int64_t> ExpertRoutes::padding_counts(std::int64_t layer, std::int64_t begin, std::int64_t end) const {
    return counts(padding_, layer, begin, end);
}

} // namespace markov
