#include "markov/graph/observation.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace markov {
namespace {

std::size_t index(const nlohmann::json & value, std::size_t count, std::string_view field) {
    if (!value.is_number_integer() || value.get<std::int64_t>() < 0 || value.get<std::uint64_t>() >= count)
        throw std::invalid_argument("undefined observation " + std::string(field));
    return value.get<std::size_t>();
}

NativeOrder order(const nlohmann::json & value) {
    if (!value.is_number_integer() || value.get<std::int64_t>() < 0) throw std::invalid_argument("native order must be a nonnegative integer");
    return value.get<NativeOrder>();
}

NativeAnchor anchor(const nlohmann::json & source, const std::vector<NativeSpan> & spans) {
    NativeAnchor result{ index(source.at("span"), spans.size(), "span"), order(source.at("order")) };
    const auto & span = spans[result.span];
    if (result.order < span.begin || result.order > span.end) throw std::invalid_argument("native anchor lies outside its declared span");
    return result;
}

} // namespace

bool NativeResource::has_payload(NativeOrder begin, NativeOrder end) const {
    const auto found = std::ranges::upper_bound(payload, begin, {}, &NativeRange::end);
    return found != payload.end() && found->begin < end;
}

NativeObservation NativeObservation::read(const nlohmann::json & source) {
    NativeObservation result;
    result.environment = source.at("environment");
    if (!result.environment.is_object()) throw std::invalid_argument("observation environment must be an object");

    for (const auto & resource : source.at("resources").get_ref<const nlohmann::json::array_t &>()) {
        NativeResource observed{ Resource::read(resource), resource.at("observed").get<bool>(), {} };
        for (const auto & interval : resource.at("payload").get_ref<const nlohmann::json::array_t &>()) {
            if (!interval.is_array() || interval.size() != 2) throw std::invalid_argument("native payload range requires two coordinates");
            NativeRange range{ order(interval[0]), order(interval[1]) };
            if (range.begin >= range.end || (!observed.payload.empty() && observed.payload.back().end > range.begin))
                throw std::invalid_argument("native payload ranges must be ordered and non-overlapping");
            observed.payload.push_back(range);
        }
        if (!observed.observed && !observed.payload.empty()) throw std::invalid_argument("unobserved resource contains claimed payload");
        result.resources.push_back(std::move(observed));
    }

    for (const auto & scope : source.at("scopes").get_ref<const nlohmann::json::array_t &>()) {
        auto info = ScopeInfo::read(scope);
        const auto identity = result.scopes.size();
        if ((identity == 0 && info.parent) || (identity != 0 && (!info.parent || *info.parent >= identity)))
            throw std::invalid_argument("observation scopes require one root and parents before children");
        result.scopes.push_back({ std::move(info), {} });
        if (scope.contains("communication")) result.scopes.back().communication = CommunicationGroup::read(scope.at("communication"));
    }
    if (result.scopes.empty()) throw std::invalid_argument("observation requires a window root scope");

    for (const auto & span : source.at("spans").get_ref<const nlohmann::json::array_t &>()) {
        NativeSpan region{ index(span.at("scope"), result.scopes.size(), "scope"),
                           index(span.at("resource"), result.resources.size(), "resource"),
                           order(span.at("begin")),
                           order(span.at("end")),
                           work_kind(span.at("kind").get<std::string>()) };
        if (region.begin > region.end || (region.begin == region.end && region.kind != WorkKind::Boundary && region.kind != WorkKind::Wait))
            throw std::invalid_argument("native work span must have distinct entry and return boundaries");
        result.spans.push_back(region);
    }

    for (std::size_t identity = 0; identity < result.scopes.size(); ++identity) {
        for (const auto & access : source.at("scopes").at(identity).at("kv_accesses").get_ref<const nlohmann::json::array_t &>())
            result.scopes[identity].kv_accesses.push_back(
                { KvAccess::read(access), anchor(access.at("start"), result.spans), anchor(access.at("finish"), result.spans) });
        for (const auto & access : source.at("scopes").at(identity).at("slot_accesses"))
            result.scopes[identity].slot_accesses.push_back(
                { SlotAccess::read(access), anchor(access.at("start"), result.spans), anchor(access.at("finish"), result.spans) });
        for (const auto & port : source.at("scopes").at(identity).at("ports").get_ref<const nlohmann::json::array_t &>()) {
            NativePort observed{ port.at("name").get<std::string>(),          direction_kind(port.at("direction").get<std::string>()),
                                 port_use(port.at("use").get<std::string>()), relation_kind(port.at("relation").get<std::string>()),
                                 anchor(port.at("at"), result.spans),         std::nullopt };
            if (observed.name.empty()) throw std::invalid_argument("scope port name must not be empty");
            if (port.contains("tensor") && !port.at("tensor").is_null()) observed.tensor = TensorObservation::read(port.at("tensor"));
            if (port.contains("wait")) {
                const auto & wait = port.at("wait");
                observed.wait =
                    StreamWait<NativeAnchor>{ index(wait.at("resource"), result.resources.size(), "wait stream"), anchor(wait.at("begin"), result.spans) };
            }
            if (port.contains("rendezvous")) observed.rendezvous = anchor(port.at("rendezvous"), result.spans);
            if (observed.use == PortUse::Signal) observed.signal = port.at("signal").get<std::string>();
            result.scopes[identity].ports.push_back(std::move(observed));
        }
    }

    for (const auto & dependency : source.at("dependencies").get_ref<const nlohmann::json::array_t &>())
        result.dependencies.push_back({ anchor(dependency.at("source"), result.spans),
                                        anchor(dependency.at("target"), result.spans),
                                        relation_kind(dependency.at("kind").get<std::string>()) });
    if (source.contains("measurements")) {
        const auto & measured = source.at("measurements");
        const auto & window = measured.at("window_ns");
        if (!window.is_array() || window.size() != 2) throw std::invalid_argument("native measurement window requires two nanosecond boundaries");
        NativeMeasurements clocks{ measured.at("clock").get<std::string>(), order(window[0]), order(window[1]), {} };
        if (clocks.clock.empty() || clocks.window_begin_ns > clocks.window_end_ns) throw std::invalid_argument("invalid native measurement clock or window");
        for (const auto & point : measured.at("coordinates")) {
            const auto key = std::pair{ index(point.at("resource"), result.resources.size(), "measurement resource"), order(point.at("order")) };
            const auto time = point.at("time_ns").is_null() ? std::nullopt : std::optional(order(point.at("time_ns")));
            if (!clocks.coordinates.emplace(key, time).second) throw std::invalid_argument("native coordinate has competing time measurements");
        }
        result.measurements = std::move(clocks);
    }
    return result;
}

} // namespace markov
