#include "markov/graph/state.hpp"

#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov {
namespace {

std::int64_t nonnegative(const nlohmann::json & value) {
    if (!value.is_number_integer() || value.get<std::int64_t>() < 0) throw std::invalid_argument("state coordinates must be nonnegative integers");
    return value.get<std::int64_t>();
}

} // namespace

StateAccess state_access(std::string_view name) {
    if (name == "read") return StateAccess::Read;
    if (name == "write") return StateAccess::Write;
    throw std::invalid_argument("unknown state access: " + std::string(name));
}

std::string_view state_access_name(StateAccess access) { return access == StateAccess::Read ? "read" : "write"; }

TokenRegion TokenRegion::read(const nlohmann::json & source) {
    TokenRegion result{ source.at("request").get<std::string>(), nonnegative(source.at("begin")), nonnegative(source.at("end")) };
    if (result.request.empty() || result.begin > result.end) throw std::invalid_argument("invalid request token region");
    return result;
}

nlohmann::json TokenRegion::document() const {
    return {
        { "request", request },
        {   "begin",   begin },
        {     "end",     end }
    };
}

TokenRangeRule TokenRangeRule::read(const nlohmann::json & source) {
    TokenRangeRule result{ Expression::read(source.at("begin")), Expression::read(source.at("end")) };
    for (const auto * expression : { &result.begin, &result.end }) {
        for (const auto & variable : expression->variables()) {
            if (variable != "context_tokens" && variable != "new_tokens")
                throw std::invalid_argument("token range uses an unknown member feature: " + variable);
        }
    }
    return result;
}

nlohmann::json TokenRangeRule::document() const {
    return {
        { "begin", begin.document() },
        {   "end",   end.document() }
    };
}

std::vector<TokenRegion> TokenRangeRule::evaluate(const nlohmann::json & work) const {
    std::vector<TokenRegion> regions;
    std::set<std::string> requests;
    for (const auto & member : work.at("members").get_ref<const nlohmann::json::array_t &>()) {
        Parameters parameters{
            { "context_tokens", nonnegative(member.at("context_tokens")) },
            {     "new_tokens",     nonnegative(member.at("new_tokens")) }
        };
        TokenRegion region{ member.at("id").get<std::string>(), begin.evaluate(parameters), end.evaluate(parameters) };
        if (region.request.empty() || !requests.insert(region.request).second || region.begin < 0 || region.end < region.begin)
            throw std::invalid_argument("token range produces an invalid request region");
        regions.push_back(std::move(region));
    }
    return regions;
}

KvSlice KvSlice::read(const nlohmann::json & source) {
    KvSlice result{ nonnegative(source.at("token_bytes")), nonnegative(source.at("begin")), nonnegative(source.at("end")) };
    result.validate();
    return result;
}

nlohmann::json KvSlice::document() const {
    return {
        { "token_bytes", token_bytes },
        {       "begin",       begin },
        {         "end",         end }
    };
}

void KvSlice::validate() const {
    if (token_bytes <= 0 || begin < 0 || begin >= end || end > token_bytes)
        throw std::invalid_argument("KV slice must be a nonempty byte range within one token");
}

std::vector<std::pair<std::int64_t, std::int64_t>> KvSlice::ranges(std::int64_t token_begin, std::int64_t token_end) const {
    if (token_begin < 0 || token_begin > token_end || token_end > std::numeric_limits<std::int64_t>::max() / token_bytes)
        throw std::invalid_argument("KV token range exceeds the byte address space");
    if (token_begin == token_end) return {};

    // Full-token accesses remain one interval; only partial payloads need per-token slices.
    if (begin == 0 && end == token_bytes)
        return {
            { token_begin * token_bytes, token_end * token_bytes }
        };
    std::vector<std::pair<std::int64_t, std::int64_t>> result;
    for (auto token = token_begin; token < token_end; ++token) result.emplace_back(token * token_bytes + begin, token * token_bytes + end);
    return result;
}

KvAccess KvAccess::read(const nlohmann::json & source) {
    if (source.at("space") != "kv") throw std::invalid_argument("state access is not in the KV address space");
    KvAccess result{ source.at("pool").get<std::string>(),
                     nonnegative(source.at("layer")),
                     state_access(source.at("access").get<std::string>()),
                     KvSlice::read(source.at("slice")),
                     {},
                     {} };
    if (result.pool.empty()) throw std::invalid_argument("KV access requires an explicit pool binding");
    std::set<std::string> requests;
    for (const auto & region : source.at("regions").get_ref<const nlohmann::json::array_t &>()) {
        auto value = TokenRegion::read(region);
        if (!requests.insert(value.request).second) throw std::invalid_argument("KV access repeats a request region");
        result.regions.push_back(std::move(value));
    }
    if (source.contains("rule")) result.rule = TokenRangeRule::read(source.at("rule"));
    return result;
}

nlohmann::json KvAccess::document() const {
    nlohmann::json result{
        {   "space",                      "kv" },
        {    "pool",                      pool },
        {   "layer",                     layer },
        {  "access", state_access_name(access) },
        {   "slice",          slice.document() },
        { "regions",   nlohmann::json::array() }
    };
    for (const auto & region : regions) result["regions"].push_back(region.document());
    if (rule) result["rule"] = rule->document();
    return result;
}

SlotAccess SlotAccess::read(const nlohmann::json & source) {
    if (source.at("space") != "slot") throw std::invalid_argument("state access is not in the request-slot address space");
    SlotAccess result{ source.at("pool").get<std::string>(),
                       source.at("field").get<std::string>(),
                       state_access(source.at("access").get<std::string>()),
                       source.at("members").get<std::string>(),
                       source.at("requests").get<std::vector<std::string>>(),
                       source.value("publication", std::string{}) };
    if (result.pool.empty() || result.field.empty() || result.members.empty())
        throw std::invalid_argument("slot access requires a pool, field and member binding");
    if (!result.publication.empty() && result.access != StateAccess::Write)
        throw std::invalid_argument("only slot writes can declare a replicated publication");
    std::set<std::string> seen;
    for (const auto & request : result.requests) {
        if (request.empty() || !seen.insert(request).second) throw std::invalid_argument("slot access repeats or omits a request identity");
    }
    return result;
}

nlohmann::json SlotAccess::document() const {
    nlohmann::json result{
        {    "space",                    "slot" },
        {     "pool",                      pool },
        {    "field",                     field },
        {   "access", state_access_name(access) },
        {  "members",                   members },
        { "requests",                  requests }
    };
    if (!publication.empty()) result["publication"] = publication;
    return result;
}

} // namespace markov
